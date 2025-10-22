#ifndef FILE_LIST_API_H
#define FILE_LIST_API_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../log/log.h"
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

/**
 * @brief 文件信息结构
 */
struct FileInfo {
    std::string name;
    size_t size;
    time_t mtime;  // 修改时间

    // 排序：按修改时间降序（最新的在前）
    bool operator<(const FileInfo& other) const {
        return mtime > other.mtime;
    }
};

/**
 * @brief 扫描目录获取文件列表
 *
 * @param upload_dir 要扫描的目录路径
 * @param files 输出参数，存储文件信息列表
 * @return 是否成功扫描
 */
inline bool scan_directory_files(const char* upload_dir, std::vector<FileInfo>& files) {
    fprintf(stderr, "[DEBUG FILE_LIST] Scanning directory: %s\n", upload_dir);
    fflush(stderr);

    DIR* dir = opendir(upload_dir);
    if (!dir) {
        LOG_ERROR("Failed to open directory %s: %s", upload_dir, strerror(errno));
        return false;
    }

    struct dirent* entry;
    int file_count = 0;

    while ((entry = readdir(dir)) != nullptr) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // 构建完整路径
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", upload_dir, entry->d_name);

        // 获取文件信息
        struct stat file_stat;
        if (stat(full_path, &file_stat) != 0) {
            LOG_WARN("Failed to stat file %s: %s", full_path, strerror(errno));
            continue;
        }

        // 只包含普通文件（不包含目录和特殊文件）
        if (!S_ISREG(file_stat.st_mode)) {
            continue;
        }

        FileInfo file_info;
        file_info.name = entry->d_name;
        file_info.size = file_stat.st_size;
        file_info.mtime = file_stat.st_mtime;

        files.push_back(file_info);
        file_count++;

        fprintf(stderr, "[DEBUG FILE_LIST] Found file: %s (size=%zu, mtime=%ld)\n",
                entry->d_name, file_info.size, (long)file_info.mtime);
        fflush(stderr);
    }

    closedir(dir);

    // 排序：按修改时间降序
    std::sort(files.begin(), files.end());

    fprintf(stderr, "[DEBUG FILE_LIST] Scanned %d files\n", file_count);
    fflush(stderr);

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
 */
inline std::string build_file_list_json(const std::vector<FileInfo>& files) {
    std::string json = "{\"success\":true,\"count\":";
    json += std::to_string(files.size());
    json += ",\"files\":[";

    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            json += ",";
        }

        json += "{\"name\":\"";
        json += json_escape(files[i].name);
        json += "\",\"size\":";
        json += std::to_string(files[i].size);
        json += ",\"mtime\":";
        json += std::to_string(files[i].mtime);
        json += "}";
    }

    json += "]}";
    return json;
}

/**
 * @brief 协程动态文件列表API处理器
 *
 * 功能特性：
 * - 扫描指定目录获取所有文件
 * - 返回JSON格式的文件列表
 * - 包含文件名、大小、修改时间
 * - 按修改时间降序排序
 * - 自动过滤目录和特殊文件
 *
 * API路径: GET /api/files
 *
 * 使用示例：
 * @code
 * bool success = co_await handle_file_list_api(
 *     sockfd, io_mgr, "./root/uploads"
 * );
 * @endcode
 */
inline Task<bool> handle_file_list_api(
    int sockfd,
    IoUringManager* io_mgr,
    const char* upload_dir = "./root/uploads"
) {
    fprintf(stderr, "[DEBUG FILE_LIST] Entered handle_file_list_api, sockfd=%d\n", sockfd);
    fflush(stderr);

    try {
        // 1. 扫描目录获取文件列表
        std::vector<FileInfo> files;
        bool scan_success = scan_directory_files(upload_dir, files);

        std::string json_body;

        if (!scan_success) {
            // 扫描失败（目录不存在或权限不足）
            json_body = "{\"success\":false,\"message\":\"无法读取目录\"}";
            LOG_ERROR("Failed to scan directory: %s", upload_dir);
        } else {
            // 2. 构建JSON响应
            json_body = build_file_list_json(files);
            LOG_INFO("File list API: returning %zu files", files.size());
        }

        fprintf(stderr, "[DEBUG FILE_LIST] JSON body length: %zu\n", json_body.length());
        fflush(stderr);

        // 3. 发送HTTP响应
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
