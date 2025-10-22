#ifndef FILE_SEARCH_API_H
#define FILE_SEARCH_API_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../http/file_db_manager.h"
#include "../log/log.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

/**
 * @brief URL解码工具函数
 *
 * 将 %20 → 空格, %E4%B8%AD → 中文字符等
 */
inline std::string url_decode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.length());

    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // 解析%XX格式
            char hex[3] = {encoded[i+1], encoded[i+2], '\0'};
            char ch = static_cast<char>(strtol(hex, nullptr, 16));
            decoded += ch;
            i += 2;
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }

    return decoded;
}

/**
 * @brief 解析URL查询字符串
 *
 * 格式: key1=value1&key2=value2&...
 *
 * 示例: "q=test&ext=.pdf&size_min=1024"
 */
inline void parse_query_string(const std::string& query, SearchCriteria& criteria) {
    size_t start = 0;

    while (start < query.length()) {
        // 找到下一个 & 或结束
        size_t end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.length();
        }

        // 提取 key=value 对
        size_t equal_pos = query.find('=', start);
        if (equal_pos != std::string::npos && equal_pos < end) {
            std::string key = query.substr(start, equal_pos - start);
            std::string value = url_decode(query.substr(equal_pos + 1, end - equal_pos - 1));

            // 根据key设置criteria字段
            if (key == "q" || key == "keyword") {
                criteria.keyword = value;
            } else if (key == "ext" || key == "extension") {
                // 确保扩展名以点号开头
                if (!value.empty() && value[0] != '.') {
                    criteria.extension = "." + value;
                } else {
                    criteria.extension = value;
                }
            } else if (key == "size_min") {
                criteria.size_min = strtoull(value.c_str(), nullptr, 10);
            } else if (key == "size_max") {
                criteria.size_max = strtoull(value.c_str(), nullptr, 10);
            } else if (key == "date_from") {
                criteria.date_from = strtol(value.c_str(), nullptr, 10);
            } else if (key == "date_to") {
                criteria.date_to = strtol(value.c_str(), nullptr, 10);
            }
            // 新增参数解析
            else if (key == "file_type" || key == "type") {
                criteria.file_type = value;
            } else if (key == "sort_by" || key == "sort") {
                criteria.sort_by = value;
            } else if (key == "sort_order" || key == "order") {
                criteria.sort_order = value;
            } else if (key == "regex_mode" || key == "regex") {
                criteria.regex_mode = (value == "1" || value == "true" || value == "on");
            } else if (key == "limit") {
                criteria.limit = atoi(value.c_str());
            }
        }

        start = end + 1;
    }
}

/**
 * @brief 构建搜索结果的JSON响应
 *
 * JSON格式：
 * {
 *   "success": true,
 *   "count": 3,
 *   "keyword": "test",
 *   "files": [
 *     {
 *       "id": 1,
 *       "filename": "test.pdf",
 *       "file_path": "./root/uploads/test.pdf",
 *       "file_size": 2457600,
 *       "file_hash": "d41d8cd98f00b204e9800998ecf8427e",
 *       "file_type": "pdf",
 *       "extension": ".pdf",
 *       "upload_time": 1729612800
 *     },
 *     ...
 *   ]
 * }
 */
inline std::string build_search_json(const std::vector<FileRecord>& files,
                                     const SearchCriteria& criteria) {
    std::string json = "{\"success\":true,\"count\":";
    json += std::to_string(files.size());

    // 添加搜索条件信息
    if (!criteria.keyword.empty()) {
        json += ",\"keyword\":\"";
        json += json_escape(criteria.keyword);
        json += "\"";
    }

    if (!criteria.extension.empty()) {
        json += ",\"extension\":\"";
        json += json_escape(criteria.extension);
        json += "\"";
    }

    json += ",\"files\":[";

    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            json += ",";
        }

        json += "{";
        json += "\"id\":";
        json += std::to_string(files[i].id);
        json += ",\"filename\":\"";
        json += json_escape(files[i].filename);
        json += "\",\"file_path\":\"";
        json += json_escape(files[i].file_path);
        json += "\",\"file_size\":";
        json += std::to_string(files[i].file_size);

        // file_hash可能为NULL
        if (!files[i].file_hash.empty()) {
            json += ",\"file_hash\":\"";
            json += json_escape(files[i].file_hash);
            json += "\"";
        } else {
            json += ",\"file_hash\":null";
        }

        json += ",\"file_type\":\"";
        json += json_escape(files[i].file_type);
        json += "\",\"extension\":\"";
        json += json_escape(files[i].extension);
        json += "\",\"upload_time\":";
        json += std::to_string(files[i].upload_time);
        json += "}";
    }

    json += "]}";
    return json;
}

/**
 * @brief 协程数据库驱动文件搜索API处理器
 *
 * 功能特性：
 * - 从MySQL数据库查询文件记录
 * - 支持多条件组合搜索：
 *   * 关键字匹配（文件名）
 *   * 扩展名过滤
 *   * 文件大小范围
 *   * 上传时间范围
 * - 返回JSON格式的搜索结果
 * - 自动SQL注入防护
 * - 索引优化查询性能
 *
 * API路径: GET /api/search?q=keyword&ext=.pdf&size_min=1024&size_max=1048576
 *
 * 查询参数：
 * - q/keyword: 文件名关键字（模糊匹配）
 * - ext/extension: 文件扩展名（如 .pdf）
 * - size_min: 最小文件大小（字节）
 * - size_max: 最大文件大小（字节）
 * - date_from: 起始时间（Unix时间戳）
 * - date_to: 结束时间（Unix时间戳）
 *
 * 使用示例：
 * @code
 * bool success = co_await handle_file_search_api(
 *     sockfd, io_mgr, "q=test&ext=.pdf", connPool
 * );
 * @endcode
 */
inline Task<bool> handle_file_search_api(
    int sockfd,
    IoUringManager* io_mgr,
    const std::string& query_string,
    connection_pool* connPool
) {
    fprintf(stderr, "[DEBUG SEARCH_API] Entered handle_file_search_api, sockfd=%d\n", sockfd);
    fprintf(stderr, "[DEBUG SEARCH_API] Query string: %s\n", query_string.c_str());
    fflush(stderr);

    try {
        // 1. 解析查询参数
        SearchCriteria criteria;
        parse_query_string(query_string, criteria);

        fprintf(stderr, "[DEBUG SEARCH_API] Parsed criteria: keyword='%s', ext='%s'\n",
                criteria.keyword.c_str(), criteria.extension.c_str());
        fflush(stderr);

        // 2. 获取数据库连接
        MYSQL* mysql = nullptr;
        connectionRAII mysqlcon(&mysql, connPool);

        if (!mysql) {
            LOG_ERROR("Failed to get MySQL connection for search API");

            std::string error_json = "{\"success\":false,\"message\":\"数据库连接失败\"}";
            std::string http_response = "HTTP/1.1 500 Internal Server Error\r\n";
            http_response += "Content-Type: application/json; charset=utf-8\r\n";
            http_response += "Content-Length: ";
            http_response += std::to_string(error_json.length());
            http_response += "\r\n";
            http_response += "Connection: close\r\n";
            http_response += "\r\n";
            http_response += error_json;

            co_await async_write(io_mgr, sockfd, http_response.c_str(), http_response.length());
            co_return false;
        }

        // 3. 执行数据库搜索
        std::vector<FileRecord> files = FileDBManager::search_files(mysql, criteria, 100);

        fprintf(stderr, "[DEBUG SEARCH_API] Database returned %zu files\n", files.size());
        fflush(stderr);

        LOG_INFO("Search API: found %zu files (keyword='%s', ext='%s')",
                 files.size(), criteria.keyword.c_str(), criteria.extension.c_str());

        // 4. 构建JSON响应
        std::string json_body = build_search_json(files, criteria);

        fprintf(stderr, "[DEBUG SEARCH_API] JSON body length: %zu\n", json_body.length());
        fflush(stderr);

        // 5. 发送HTTP响应
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

        fprintf(stderr, "[DEBUG SEARCH_API] Sending response, total length: %zu\n",
                http_response.length());
        fflush(stderr);

        // 6. 异步写入响应（处理部分写入）
        size_t total_written = 0;
        const char* data_ptr = http_response.c_str();
        size_t data_len = http_response.length();

        while (total_written < data_len) {
            size_t remaining = data_len - total_written;

            ssize_t written = co_await async_write(io_mgr, sockfd,
                                                     data_ptr + total_written,
                                                     remaining);

            if (written <= 0) {
                LOG_ERROR("Failed to send search response: %zd", written);
                co_return false;
            }

            total_written += written;

            if (written < (ssize_t)remaining) {
                fprintf(stderr, "[DEBUG SEARCH_API] Partial write: %zd/%zu, continuing...\n",
                        written, remaining);
                fflush(stderr);
            }
        }

        fprintf(stderr, "[DEBUG SEARCH_API] Response sent successfully, total=%zu bytes\n",
                total_written);
        fflush(stderr);

        co_return true;

    } catch (const IoError& e) {
        fprintf(stderr, "[DEBUG SEARCH_API] Caught IoError: %s\n", e.what());
        fflush(stderr);
        LOG_ERROR("Search API failed with I/O error: %s", e.what());
        co_return false;

    } catch (const std::exception& e) {
        fprintf(stderr, "[DEBUG SEARCH_API] Caught exception: %s\n", e.what());
        fflush(stderr);
        LOG_ERROR("Search API failed with exception: %s", e.what());
        co_return false;
    }
}

#endif // FILE_SEARCH_API_H
