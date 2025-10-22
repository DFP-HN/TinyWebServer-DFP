#ifndef FILE_LIST_API_H
#define FILE_LIST_API_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../http/file_db_manager.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>

/**
 * @brief 从数据库加载文件列表
 *
 * @param mysql MySQL连接
 * @param files 输出参数，存储文件记录列表
 * @return 是否成功查询
 */
inline bool load_files_from_database(MYSQL* mysql, std::vector<FileRecord>& files) {
    fprintf(stderr, "[DEBUG FILE_LIST] Querying files from database\n");
    fflush(stderr);

    if (!mysql) {
        LOG_ERROR("Invalid MySQL connection");
        return false;
    }

    // 查询所有文件（按上传时间降序，最多1000条）
    files = FileDBManager::query_all_files(mysql, "upload_time DESC", 1000);

    fprintf(stderr, "[DEBUG FILE_LIST] Database returned %zu files\n", files.size());
    fflush(stderr);

    LOG_INFO("Loaded %zu files from database", files.size());
    return true;
}

/**
 * @brief JSON字符串转义（处理文件名中的特殊字符）
 *
 * 转义规则：
 * - " → \"
 * - \ → \\
 * - / → \/
 * - \n, \r, \t 等控制字符
 */
inline std::string json_escape(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.length() + 20);

    for (char c : str) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '/':  escaped += "\\/"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                // 处理其他控制字符
                if (c >= 0 && c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
                break;
        }
    }

    return escaped;
}

/**
 * @brief 构建文件列表的JSON响应
 *
 * JSON格式：
 * {
 *   "success": true,
 *   "count": 3,
 *   "files": [
 *     {"name": "file.pdf", "size": 2457600, "mtime": 1729612800},
 *     ...
 *   ]
 * }
 *
 * 注意：为保持与前端兼容，使用 name/size/mtime 字段名
 */
inline std::string build_file_list_json(const std::vector<FileRecord>& files) {
    std::string json = "{\"success\":true,\"count\":";
    json += std::to_string(files.size());
    json += ",\"files\":[";

    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            json += ",";
        }

        json += "{\"name\":\"";
        json += json_escape(files[i].filename);  // FileRecord.filename → name
        json += "\",\"size\":";
        json += std::to_string(files[i].file_size);  // FileRecord.file_size → size
        json += ",\"mtime\":";
        json += std::to_string(files[i].upload_time);  // FileRecord.upload_time → mtime
        json += "}";
    }

    json += "]}";
    return json;
}

/**
 * @brief 协程数据库驱动文件列表API处理器
 *
 * 功能特性：
 * - 从MySQL数据库查询所有文件记录
 * - 返回JSON格式的文件列表
 * - 包含文件名、大小、上传时间
 * - 按上传时间降序排序（最新的在前）
 * - 支持最多1000条记录
 * - 索引优化查询性能
 *
 * API路径: GET /api/files
 *
 * 使用示例：
 * @code
 * bool success = co_await handle_file_list_api(
 *     sockfd, io_mgr, connPool
 * );
 * @endcode
 */
inline Task<bool> handle_file_list_api(
    int sockfd,
    IoUringManager* io_mgr,
    connection_pool* connPool
) {
    fprintf(stderr, "[DEBUG FILE_LIST] Entered handle_file_list_api, sockfd=%d\n", sockfd);
    fflush(stderr);

    try {
        // 1. 获取数据库连接
        MYSQL* mysql = nullptr;
        connectionRAII mysqlcon(&mysql, connPool);

        std::string json_body;

        if (!mysql) {
            // 数据库连接失败
            json_body = "{\"success\":false,\"message\":\"数据库连接失败\"}";
            LOG_ERROR("Failed to get MySQL connection for file list API");
        } else {
            // 2. 从数据库查询文件列表
            std::vector<FileRecord> files;
            bool query_success = load_files_from_database(mysql, files);

            if (!query_success) {
                json_body = "{\"success\":false,\"message\":\"查询文件列表失败\"}";
                LOG_ERROR("Failed to query files from database");
            } else {
                // 3. 构建JSON响应
                json_body = build_file_list_json(files);
                LOG_INFO("File list API: returning %zu files from database", files.size());
            }
        }

        fprintf(stderr, "[DEBUG FILE_LIST] JSON body length: %zu\n", json_body.length());
        fflush(stderr);

        // 4. 发送HTTP响应
        std::string http_response;
        http_response.reserve(json_body.length() + 256);

        http_response = "HTTP/1.1 200 OK\r\n";
        http_response += "Content-Type: application/json; charset=utf-8\r\n";
        http_response += "Content-Length: ";
        http_response += std::to_string(json_body.length());
        http_response += "\r\n";
        http_response += "Connection: close\r\n";
        http_response += "Access-Control-Allow-Origin: *\r\n";
        http_response += "\r\n";
        http_response += json_body;

        fprintf(stderr, "[DEBUG FILE_LIST] Sending response, total length: %zu\n",
                http_response.length());
        fflush(stderr);

        // 4. 异步写入响应（处理部分写入）
        size_t total_written = 0;
        const char* data_ptr = http_response.c_str();
        size_t data_len = http_response.length();

        while (total_written < data_len) {
            size_t remaining = data_len - total_written;

            ssize_t written = co_await async_write(io_mgr, sockfd,
                                                     data_ptr + total_written,
                                                     remaining);

            if (written <= 0) {
                LOG_ERROR("Failed to send file list response: %zd", written);
                co_return false;
            }

            total_written += written;

            // 部分写入日志
            if (written < (ssize_t)remaining) {
                fprintf(stderr, "[DEBUG FILE_LIST] Partial write: %zd/%zu, continuing...\n",
                        written, remaining);
                fflush(stderr);
            }
        }

        fprintf(stderr, "[DEBUG FILE_LIST] Response sent successfully, total=%zu bytes\n",
                total_written);
        fflush(stderr);

        co_return true;

    } catch (const IoError& e) {
        fprintf(stderr, "[DEBUG FILE_LIST] Caught IoError: %s\n", e.what());
        fflush(stderr);
        LOG_ERROR("File list API failed with I/O error: %s", e.what());
        co_return false;

    } catch (const std::exception& e) {
        fprintf(stderr, "[DEBUG FILE_LIST] Caught exception: %s\n", e.what());
        fflush(stderr);
        LOG_ERROR("File list API failed with exception: %s", e.what());
        co_return false;
    }
}

#endif // FILE_LIST_API_H
