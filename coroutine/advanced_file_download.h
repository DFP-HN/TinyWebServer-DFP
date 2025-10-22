#ifndef ADVANCED_FILE_DOWNLOAD_H
#define ADVANCED_FILE_DOWNLOAD_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../http/http_range.h"
#include "../log/log.h"
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <algorithm>
#include <memory>
#include <cstring>

/**
 * @brief 高级文件下载配置
 */
struct AdvancedDownloadConfig {
    size_t chunk_size = 128 * 1024;                          // 128KB 分块传输
    size_t max_download_rate = 10 * 1024 * 1024;            // 10MB/s 限速
    bool enable_rate_limit = false;                          // 启用速率限制
    size_t progress_interval = 1 * 1024 * 1024;              // 每 1MB 回调一次
    const char* download_dir = "./root/uploads";             // 下载目录
};

/**
 * @brief 文件下载结果
 */
struct AdvancedDownloadResult {
    bool success = false;
    std::string filename;
    size_t bytes_sent = 0;
    double download_time_seconds = 0.0;
    std::string error_message;
    bool is_partial = false;                // 是否 Range 请求（206）
    size_t total_file_size = 0;             // 文件总大小
};

/**
 * @brief 下载进度信息
 */
struct DownloadProgress {
    size_t bytes_sent;
    size_t total_bytes;
    double percentage;
    double download_rate;       // 字节/秒
    time_t estimated_remaining;  // 预计剩余秒数
};

/**
 * @brief 下载进度回调
 */
using DownloadProgressCallback = std::function<void(const DownloadProgress&)>;

/**
 * @brief 安全的文件路径验证
 *
 * 防止目录遍历攻击（如 ../../etc/passwd）
 */
inline bool is_safe_filename(const char* filename) {
    if (!filename || *filename == '\0') {
        return false;
    }

    // 不允许包含 ".." 或绝对路径
    if (strstr(filename, "..") != nullptr || filename[0] == '/') {
        LOG_WARN("Unsafe filename detected: %s", filename);
        return false;
    }

    // 不允许路径分隔符
    if (strchr(filename, '/') != nullptr || strchr(filename, '\\') != nullptr) {
        LOG_WARN("Path separator in filename: %s", filename);
        return false;
    }

    return true;
}

/**
 * @brief URL编码（用于RFC 2231文件名编码）
 */
inline std::string url_encode_rfc2231(const char* str) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;

    for (const char* p = str; *p; ++p) {
        unsigned char c = *p;
        // RFC 2231: 只保留字母、数字和部分特殊字符
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            // 其他字符用 %XX 编码
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0F];
        }
    }

    return encoded;
}

/**
 * @brief 获取文件 MIME 类型
 */
inline const char* get_mime_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) {
        return "application/octet-stream";
    }

    // 常见 MIME 类型
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".zip") == 0) return "application/zip";
    if (strcmp(ext, ".tar") == 0) return "application/x-tar";
    if (strcmp(ext, ".gz") == 0) return "application/gzip";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(ext, ".mov") == 0) return "video/quicktime";

    return "application/octet-stream";
}

/**
 * @brief 发送 HTTP 响应头（协程版本）
 */
inline Task<bool> send_download_response_header(
    IoUringManager* io_mgr,
    int sockfd,
    int status_code,
    const char* status_text,
    size_t content_length,
    const char* content_type,
    const char* filename,
    const HttpRange* range = nullptr,
    size_t total_file_size = 0
) {
    char header[2048];
    int len = 0;

    // 状态行
    len += snprintf(header + len, sizeof(header) - len,
                    "HTTP/1.1 %d %s\r\n", status_code, status_text);

    // Content-Type
    len += snprintf(header + len, sizeof(header) - len,
                    "Content-Type: %s\r\n", content_type);

    // Content-Length
    len += snprintf(header + len, sizeof(header) - len,
                    "Content-Length: %zu\r\n", content_length);

    // Content-Disposition (提示浏览器下载，支持中文文件名 RFC 2231)
    // 同时提供 filename (ASCII fallback) 和 filename* (UTF-8编码)
    std::string encoded_filename = url_encode_rfc2231(filename);
    len += snprintf(header + len, sizeof(header) - len,
                    "Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''%s\r\n",
                    filename, encoded_filename.c_str());

    // Content-Range (如果是 Range 请求)
    if (range && range->is_range_request) {
        len += range->format_content_range(total_file_size, header + len, sizeof(header) - len);
    }

    // Accept-Ranges (支持断点续传)
    len += snprintf(header + len, sizeof(header) - len,
                    "Accept-Ranges: bytes\r\n");

    // Connection
    len += snprintf(header + len, sizeof(header) - len,
                    "Connection: close\r\n");

    // 空行
    len += snprintf(header + len, sizeof(header) - len, "\r\n");

    try {
        ssize_t written = co_await async_write(io_mgr, sockfd, header, len);
        if (written != len) {
            LOG_WARN("Response header write incomplete: %zd/%d", written, len);
            co_return false;
        }
        co_return true;
    } catch (const IoError& e) {
        LOG_ERROR("Failed to send response header: %s", e.what());
        co_return false;
    }
}

/**
 * @brief 发送 HTTP 错误响应（协程版本）
 */
inline Task<void> send_error_response(
    IoUringManager* io_mgr,
    int sockfd,
    int status_code,
    const char* status_text,
    const char* error_message
) {
    char response[2048];
    char body[1024];

    snprintf(body, sizeof(body),
             "<html><body><h1>%d %s</h1><p>%s</p></body></html>",
             status_code, status_text, error_message);

    int len = snprintf(response, sizeof(response),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       status_code, status_text, strlen(body), body);

    try {
        co_await async_write(io_mgr, sockfd, response, len);
    } catch (const IoError& e) {
        LOG_ERROR("Failed to send error response: %s", e.what());
    }
}

/**
 * @brief 高级协程文件下载处理器
 *
 * 功能特性：
 * - 流式下载大文件（固定内存占用 ~150KB）
 * - 支持断点续传（HTTP Range 请求）
 * - 实时进度跟踪
 * - 速率限制
 * - 自动 MIME 类型检测
 *
 * 使用示例：
 * @code
 * AdvancedDownloadConfig config;
 * config.enable_rate_limit = false;
 *
 * HttpRange range;
 * range.parse_range_header("bytes=0-1023", file_size);
 *
 * AdvancedDownloadResult result = co_await handle_advanced_file_download(
 *     sockfd, io_mgr, "/path/to/file.pdf", config,
 *     [](const DownloadProgress& progress) {
 *         printf("Progress: %.1f%%\n", progress.percentage);
 *     },
 *     &range
 * );
 * @endcode
 */
inline Task<AdvancedDownloadResult> handle_advanced_file_download(
    int sockfd,
    IoUringManager* io_mgr,
    const char* file_path,
    const AdvancedDownloadConfig& config = AdvancedDownloadConfig(),
    DownloadProgressCallback progress_callback = nullptr,
    const HttpRange* range = nullptr
) {
    fprintf(stderr, "[DEBUG DOWNLOAD] Entered handle_advanced_file_download, sockfd=%d, file_path=%s\n",
            sockfd, file_path);
    fflush(stderr);

    AdvancedDownloadResult result;
    result.success = false;
    result.bytes_sent = 0;

    time_t start_time = time(NULL);

    try {
        // 1. 安全检查：验证文件路径
        const char* filename = strrchr(file_path, '/');
        if (filename) {
            filename++;  // 跳过 '/'
        } else {
            filename = file_path;
        }

        result.filename = filename;

        fprintf(stderr, "[DEBUG DOWNLOAD] Step 1: Checking file path safety\n");
        fflush(stderr);

        if (!is_safe_filename(filename)) {
            result.error_message = "Unsafe filename";
            LOG_ERROR("Unsafe filename: %s", filename);
            co_await send_error_response(io_mgr, sockfd, 403, "Forbidden",
                                          "Access to this file is forbidden.");
            co_return result;
        }

        // 2. 打开文件并获取信息
        fprintf(stderr, "[DEBUG DOWNLOAD] Step 2: Opening file: %s\n", file_path);
        fflush(stderr);

        int file_fd = open(file_path, O_RDONLY);
        if (file_fd < 0) {
            result.error_message = "File not found";
            LOG_ERROR("Failed to open file %s: %s", file_path, strerror(errno));
            co_await send_error_response(io_mgr, sockfd, 404, "Not Found",
                                          "The requested file was not found.");
            co_return result;
        }

        // 获取文件大小
        struct stat file_stat;
        if (fstat(file_fd, &file_stat) != 0) {
            close(file_fd);
            result.error_message = "Failed to get file info";
            LOG_ERROR("fstat failed: %s", strerror(errno));
            co_await send_error_response(io_mgr, sockfd, 500, "Internal Server Error",
                                          "Failed to get file information.");
            co_return result;
        }

        size_t file_size = file_stat.st_size;
        result.total_file_size = file_size;

        fprintf(stderr, "[DEBUG DOWNLOAD] File opened: fd=%d, size=%zu\n", file_fd, file_size);
        fflush(stderr);

        // 3. 处理 Range 请求
        size_t start_pos = 0;
        size_t end_pos = file_size - 1;
        size_t bytes_to_send = file_size;
        bool is_partial = false;

        if (range && range->is_range_request) {
            fprintf(stderr, "[DEBUG DOWNLOAD] Step 3: Processing Range request: %zu-%zu\n",
                    range->start, range->end);
            fflush(stderr);

            start_pos = range->start;
            end_pos = range->end;
            bytes_to_send = range->get_content_length(file_size);
            is_partial = true;
            result.is_partial = true;

            // 验证范围合法性
            if (start_pos >= file_size || start_pos > end_pos) {
                close(file_fd);
                result.error_message = "Invalid range";
                LOG_ERROR("Invalid range: %zu-%zu/%zu", start_pos, end_pos, file_size);
                co_await send_error_response(io_mgr, sockfd, 416, "Range Not Satisfiable",
                                              "The requested range is not satisfiable.");
                co_return result;
            }

            LOG_INFO("Range download: %s bytes %zu-%zu/%zu", filename, start_pos, end_pos, file_size);
        } else {
            fprintf(stderr, "[DEBUG DOWNLOAD] Step 3: Full file download\n");
            fflush(stderr);
            LOG_INFO("Full download: %s (%zu bytes)", filename, file_size);
        }

        // 4. 发送 HTTP 响应头
        fprintf(stderr, "[DEBUG DOWNLOAD] Step 4: Sending response header\n");
        fflush(stderr);

        const char* mime_type = get_mime_type(filename);
        int status_code = is_partial ? 206 : 200;
        const char* status_text = is_partial ? "Partial Content" : "OK";

        bool header_sent = co_await send_download_response_header(
            io_mgr, sockfd, status_code, status_text,
            bytes_to_send, mime_type, filename,
            range, file_size
        );

        if (!header_sent) {
            close(file_fd);
            result.error_message = "Failed to send response header";
            LOG_ERROR("Failed to send response header");
            co_return result;
        }

        fprintf(stderr, "[DEBUG DOWNLOAD] Header sent successfully\n");
        fflush(stderr);

        // 5. 流式传输文件内容
        fprintf(stderr, "[DEBUG DOWNLOAD] Step 5: Starting file transfer\n");
        fflush(stderr);

        // 使用 shared_ptr 确保缓冲区在异步操作期间保持存活
        auto read_buffer = std::make_shared<std::string>();
        read_buffer->resize(config.chunk_size);

        size_t bytes_sent = 0;
        off_t file_offset = start_pos;

        while (bytes_sent < bytes_to_send) {
            // 速率限制（如果启用）
            if (config.enable_rate_limit && bytes_sent > 0) {
                time_t now = time(NULL);
                time_t elapsed = now - start_time;
                if (elapsed > 0) {
                    size_t current_rate = bytes_sent / elapsed;
                    if (current_rate > config.max_download_rate) {
                        usleep(100000);  // 100ms
                    }
                }
            }

            // 计算本次读取大小
            size_t to_read = std::min(config.chunk_size, bytes_to_send - bytes_sent);

            fprintf(stderr, "[DEBUG DOWNLOAD] Reading chunk: offset=%ld, size=%zu\n",
                    (long)file_offset, to_read);
            fflush(stderr);

            // 异步读取文件
            ssize_t n = co_await async_read(io_mgr, file_fd,
                                             &(*read_buffer)[0], to_read, file_offset);

            if (n <= 0) {
                LOG_ERROR("Read failed: %zd", n);
                result.error_message = "File read failed";
                close(file_fd);
                co_return result;
            }

            fprintf(stderr, "[DEBUG DOWNLOAD] Read %zd bytes, writing to socket\n", n);
            fflush(stderr);

            // 异步写入 socket（循环处理部分写入）
            size_t total_written = 0;
            try {
                // 循环写入，直到所有数据都发送完毕
                while (total_written < (size_t)n) {
                    size_t remaining = n - total_written;
                    const char* write_ptr = read_buffer->data() + total_written;

                    fprintf(stderr, "[DEBUG DOWNLOAD] Before async_write, sockfd=%d, remaining=%zu\n",
                            sockfd, remaining);
                    fflush(stderr);

                    ssize_t written = co_await async_write(io_mgr, sockfd, write_ptr, remaining);

                    fprintf(stderr, "[DEBUG DOWNLOAD] After async_write, written=%zd (remaining was %zu)\n",
                            written, remaining);
                    fflush(stderr);

                    if (written <= 0) {
                        fprintf(stderr, "[DEBUG DOWNLOAD] Write failed or connection closed: %zd\n", written);
                        fflush(stderr);
                        LOG_ERROR("Write failed: %zd", written);
                        result.error_message = "Socket write failed";
                        close(file_fd);
                        co_return result;
                    }

                    total_written += written;

                    // 如果是部分写入，记录日志
                    if (written < (ssize_t)remaining) {
                        fprintf(stderr, "[DEBUG DOWNLOAD] Partial write: %zd/%zu bytes, continuing...\n",
                                written, remaining);
                        fflush(stderr);
                        LOG_DEBUG("Partial write: %zd/%zu bytes, total=%zu/%zd",
                                  written, remaining, total_written, n);
                    }
                }

                fprintf(stderr, "[DEBUG DOWNLOAD] All %zu bytes written successfully\n", total_written);
                fflush(stderr);

            } catch (const IoError& e) {
                fprintf(stderr, "[DEBUG DOWNLOAD] CAUGHT IoError in write loop: %s (errno=%d)\n",
                        e.what(), errno);
                fflush(stderr);
                LOG_ERROR("Socket write exception: %s (errno=%d)", e.what(), errno);
                result.error_message = std::string("Socket write failed: ") + e.what();
                close(file_fd);
                co_return result;
            } catch (const std::exception& e) {
                fprintf(stderr, "[DEBUG DOWNLOAD] CAUGHT std::exception in write loop: %s\n", e.what());
                fflush(stderr);
                LOG_ERROR("Socket write unexpected exception: %s", e.what());
                result.error_message = std::string("Unexpected error: ") + e.what();
                close(file_fd);
                co_return result;
            } catch (...) {
                fprintf(stderr, "[DEBUG DOWNLOAD] CAUGHT unknown exception in write loop\n");
                fflush(stderr);
                LOG_ERROR("Socket write unknown exception");
                result.error_message = "Unknown error during socket write";
                close(file_fd);
                co_return result;
            }

            // 更新进度
            bytes_sent += total_written;
            file_offset += total_written;

            // 进度回调
            if (progress_callback && (bytes_sent % config.progress_interval == 0 || bytes_sent == bytes_to_send)) {
                time_t now = time(NULL);
                time_t elapsed = now - start_time;

                DownloadProgress progress;
                progress.bytes_sent = bytes_sent;
                progress.total_bytes = bytes_to_send;
                progress.percentage = (100.0 * bytes_sent) / bytes_to_send;

                if (elapsed > 0) {
                    progress.download_rate = (double)bytes_sent / elapsed;
                    if (progress.download_rate > 0) {
                        progress.estimated_remaining = (bytes_to_send - bytes_sent) / progress.download_rate;
                    } else {
                        progress.estimated_remaining = 0;
                    }
                } else {
                    progress.download_rate = 0;
                    progress.estimated_remaining = 0;
                }

                progress_callback(progress);

                LOG_INFO("Download progress: %zu/%zu bytes (%.1f%%), rate=%.2f MB/s",
                         bytes_sent, bytes_to_send, progress.percentage,
                         progress.download_rate / (1024.0 * 1024.0));
            }
        }

        // 6. 清理
        fprintf(stderr, "[DEBUG DOWNLOAD] Closing file fd=%d\n", file_fd);
        fflush(stderr);
        close(file_fd);

        // 7. 计算下载时间
        time_t end_time = time(NULL);
        result.download_time_seconds = difftime(end_time, start_time);

        result.success = true;
        result.bytes_sent = bytes_sent;

        LOG_INFO("Download completed: %s (%zu bytes, %.2fs, %.2f MB/s)",
                 filename, bytes_sent, result.download_time_seconds,
                 (bytes_sent / result.download_time_seconds) / (1024.0 * 1024.0));

    } catch (const IoError& e) {
        fprintf(stderr, "[DEBUG DOWNLOAD] Caught IoError: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("I/O error: ") + e.what();
        LOG_ERROR("Download failed with I/O error: %s", e.what());
        result.success = false;
    } catch (const std::exception& e) {
        fprintf(stderr, "[DEBUG DOWNLOAD] Caught exception: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("Exception: ") + e.what();
        LOG_ERROR("Download failed with exception: %s", e.what());
        result.success = false;
    }

    fprintf(stderr, "[DEBUG DOWNLOAD] Returning from handle_advanced_file_download, success=%d\n",
            result.success);
    fflush(stderr);

    co_return result;
}

#endif // ADVANCED_FILE_DOWNLOAD_H
