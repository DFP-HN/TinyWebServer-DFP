#ifndef ADVANCED_FILE_DELETE_H
#define ADVANCED_FILE_DELETE_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../http/file_db_manager.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>

/**
 * @brief 文件删除结果
 */
struct DeleteResult {
    bool success = false;
    std::string filename;
    std::string error_message;
};

/**
 * @brief 安全的文件路径验证（复用自advanced_file_download.h）
 *
 * 防止目录遍历攻击（如 ../../etc/passwd）
 */
inline bool is_safe_filename_for_delete(const char* filename) {
    if (!filename || *filename == '\0') {
        return false;
    }

    // 不允许包含 ".." 或绝对路径
    if (strstr(filename, "..") != nullptr || filename[0] == '/') {
        LOG_WARN("Unsafe filename detected for deletion: %s", filename);
        return false;
    }

    // 不允许路径分隔符
    if (strchr(filename, '/') != nullptr || strchr(filename, '\\') != nullptr) {
        LOG_WARN("Path separator in filename for deletion: %s", filename);
        return false;
    }

    return true;
}

/**
 * @brief 发送JSON响应（协程版本）
 */
inline Task<bool> send_json_response(
    IoUringManager* io_mgr,
    int sockfd,
    const char* json_body
) {
    char response[2048];
    size_t body_len = strlen(json_body);

    int header_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n"
        "%s",
        body_len, json_body);

    try {
        ssize_t written = co_await async_write(io_mgr, sockfd, response, header_len);
        if (written != header_len) {
            LOG_ERROR("Failed to send JSON response: %zd/%d", written, header_len);
            co_return false;
        }
        co_return true;
    } catch (const IoError& e) {
        LOG_ERROR("Failed to send JSON response: %s", e.what());
        co_return false;
    }
}

/**
 * @brief 协程文件删除处理器（支持数据库同步）
 *
 * 功能特性：
 * - 文件名安全验证（防止路径遍历攻击）
 * - 仅允许删除指定目录的文件
 * - 数据库软删除同步（设置status=0）
 * - 先删除数据库记录，再删除物理文件
 * - JSON格式响应
 * - 详细的错误信息
 * - 操作日志记录
 *
 * 使用示例：
 * @code
 * DeleteResult result = co_await handle_advanced_file_delete(
 *     sockfd, io_mgr, "test.pdf", connPool, "./root/uploads"
 * );
 * @endcode
 */
inline Task<DeleteResult> handle_advanced_file_delete(
    int sockfd,
    IoUringManager* io_mgr,
    const char* filename,
    connection_pool* connPool,
    const char* upload_dir = "./root/uploads"
) {
    fprintf(stderr, "[DEBUG DELETE] Entered handle_advanced_file_delete, sockfd=%d, filename=%s\n",
            sockfd, filename);
    fflush(stderr);

    DeleteResult result;
    result.success = false;
    result.filename = filename;

    char json_response[1024];

    try {
        // 1. 安全检查：验证文件名
        fprintf(stderr, "[DEBUG DELETE] Step 1: Validating filename safety\n");
        fflush(stderr);

        if (!is_safe_filename_for_delete(filename)) {
            result.error_message = "Unsafe filename";
            LOG_WARN("Attempted to delete unsafe filename: %s", filename);

            snprintf(json_response, sizeof(json_response),
                     "{\"success\":false,\"message\":\"文件名不安全\",\"filename\":\"%s\"}",
                     filename);
            co_await send_json_response(io_mgr, sockfd, json_response);
            co_return result;
        }

        // 2. 构建完整文件路径
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/%s", upload_dir, filename);

        fprintf(stderr, "[DEBUG DELETE] Step 2: Constructed file path: %s\n", file_path);
        fflush(stderr);

        // 3. 检查文件是否存在
        fprintf(stderr, "[DEBUG DELETE] Step 3: Checking file existence\n");
        fflush(stderr);

        struct stat file_stat;
        if (stat(file_path, &file_stat) != 0) {
            result.error_message = "File not found";
            LOG_WARN("Attempted to delete non-existent file: %s", file_path);

            snprintf(json_response, sizeof(json_response),
                     "{\"success\":false,\"message\":\"文件不存在\",\"filename\":\"%s\"}",
                     filename);
            co_await send_json_response(io_mgr, sockfd, json_response);
            co_return result;
        }

        // 4. 确保是普通文件（不是目录或特殊文件）
        if (!S_ISREG(file_stat.st_mode)) {
            result.error_message = "Not a regular file";
            LOG_WARN("Attempted to delete non-regular file: %s", file_path);

            snprintf(json_response, sizeof(json_response),
                     "{\"success\":false,\"message\":\"不是普通文件\",\"filename\":\"%s\"}",
                     filename);
            co_await send_json_response(io_mgr, sockfd, json_response);
            co_return result;
        }

        // 5. 数据库软删除（先删除数据库记录）
        fprintf(stderr, "[DEBUG DELETE] Step 5: Soft-deleting from database: %s\n", file_path);
        fflush(stderr);

        MYSQL* mysql = nullptr;
        connectionRAII mysqlcon(&mysql, connPool);

        if (!mysql) {
            // 数据库连接失败 - 记录警告，继续删除物理文件
            LOG_WARN("Failed to get MySQL connection for file deletion, continuing with physical delete");
        } else {
            // 尝试软删除数据库记录
            bool db_delete_success = FileDBManager::delete_file_by_path(mysql, file_path);

            if (!db_delete_success) {
                // 数据库删除失败 - 记录警告，继续删除物理文件
                LOG_WARN("Failed to delete file record from database: %s (continuing with physical delete)", file_path);
            } else {
                fprintf(stderr, "[DEBUG DELETE] Database record soft-deleted successfully\n");
                fflush(stderr);
                LOG_INFO("File record soft-deleted from database: %s", file_path);
            }
        }

        // 6. 执行物理文件删除
        fprintf(stderr, "[DEBUG DELETE] Step 6: Deleting physical file: %s\n", file_path);
        fflush(stderr);

        if (unlink(file_path) != 0) {
            result.error_message = std::string("Delete failed: ") + strerror(errno);
            LOG_ERROR("Failed to delete file %s: %s", file_path, strerror(errno));

            snprintf(json_response, sizeof(json_response),
                     "{\"success\":false,\"message\":\"删除失败：%s\",\"filename\":\"%s\"}",
                     strerror(errno), filename);
            co_await send_json_response(io_mgr, sockfd, json_response);
            co_return result;
        }

        // 7. 删除成功
        fprintf(stderr, "[DEBUG DELETE] File deleted successfully: %s\n", file_path);
        fflush(stderr);

        result.success = true;
        LOG_INFO("File deleted successfully: %s (requested by fd=%d)", file_path, sockfd);

        snprintf(json_response, sizeof(json_response),
                 "{\"success\":true,\"message\":\"文件删除成功\",\"filename\":\"%s\"}",
                 filename);
        co_await send_json_response(io_mgr, sockfd, json_response);

    } catch (const IoError& e) {
        fprintf(stderr, "[DEBUG DELETE] Caught IoError: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("I/O error: ") + e.what();
        LOG_ERROR("File deletion failed with I/O error: %s", e.what());
        result.success = false;

        snprintf(json_response, sizeof(json_response),
                 "{\"success\":false,\"message\":\"服务器错误\",\"filename\":\"%s\"}",
                 filename);
        co_await send_json_response(io_mgr, sockfd, json_response);

    } catch (const std::exception& e) {
        fprintf(stderr, "[DEBUG DELETE] Caught exception: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("Exception: ") + e.what();
        LOG_ERROR("File deletion failed with exception: %s", e.what());
        result.success = false;

        snprintf(json_response, sizeof(json_response),
                 "{\"success\":false,\"message\":\"服务器错误\",\"filename\":\"%s\"}",
                 filename);
        co_await send_json_response(io_mgr, sockfd, json_response);
    }

    fprintf(stderr, "[DEBUG DELETE] Returning from handle_advanced_file_delete, success=%d\n",
            result.success);
    fflush(stderr);

    co_return result;
}

#endif // ADVANCED_FILE_DELETE_H
