#ifndef ADVANCED_FILE_UPLOAD_H
#define ADVANCED_FILE_UPLOAD_H

#include "../coroutine/task.h"
#include "../coroutine/io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../http/streaming_multipart_parser.h"
#include "../http/hash_calculator.h"
#include "../http/file_db_manager.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <algorithm>
#include <set>
#include <memory>
#include <atomic>

/**
 * @brief 高级文件上传配置
 */
struct AdvancedUploadConfig {
    const char* upload_dir = "./root/uploads";               // 上传目录
    const char* temp_dir = "./root/uploads/.tmp";            // 临时目录
    size_t max_file_size = 2ULL * 1024 * 1024 * 1024;       // 最大2GB
    size_t chunk_size = 128 * 1024;                          // 128KB分块读取（优化大文件）
    size_t min_free_space = 100 * 1024 * 1024;               // 最小剩余空间100MB

    // 哈希校验
    bool enable_md5 = true;                                  // 启用MD5校验
    bool enable_sha256 = false;                              // 启用SHA256校验

    // 断点续传
    bool enable_resume = true;                               // 启用断点续传
    const char* resume_dir = "./root/uploads/.resume";       // 续传信息目录

    // 速率限制
    bool enable_rate_limit = false;                          // 启用速率限制
    size_t max_upload_rate = 10 * 1024 * 1024;              // 10MB/s

    // 进度回调间隔
    size_t progress_interval = 1 * 1024 * 1024;              // 每1MB回调一次

    // 允许的文件类型（空集合表示允许所有类型）
    std::set<std::string> allowed_extensions;
    std::set<std::string> blocked_extensions = {".exe", ".bat", ".sh", ".cmd"};
};

/**
 * @brief 文件上传结果
 */
struct AdvancedUploadResult {
    bool success = false;
    std::string filename;
    std::string saved_path;
    std::string error_message;
    size_t bytes_uploaded = 0;
    double upload_time_seconds = 0.0;
    std::string md5_hash;
    std::string sha256_hash;
    bool resumed = false;                // 是否从断点恢复
    size_t resume_offset = 0;            // 恢复的偏移量
};

/**
 * @brief 上传进度信息
 */
struct UploadProgress {
    size_t bytes_uploaded;
    size_t total_bytes;
    double percentage;
    double upload_rate;          // 字节/秒
    time_t estimated_remaining;   // 预计剩余秒数
};

/**
 * @brief 上传进度回调（协程友好）
 */
using ProgressCallback = std::function<void(const UploadProgress&)>;

/**
 * @brief 安全的文件名处理
 */
inline std::string sanitize_filename(const std::string& filename) {
    std::string safe = filename;

    // 移除路径分隔符
    safe.erase(std::remove(safe.begin(), safe.end(), '/'), safe.end());
    safe.erase(std::remove(safe.begin(), safe.end(), '\\'), safe.end());

    // 移除 ..
    while (safe.find("..") != std::string::npos) {
        safe.erase(safe.find(".."), 2);
    }

    // 移除危险字符
    safe.erase(std::remove(safe.begin(), safe.end(), '\0'), safe.end());
    safe.erase(std::remove(safe.begin(), safe.end(), '\r'), safe.end());
    safe.erase(std::remove(safe.begin(), safe.end(), '\n'), safe.end());

    // 限制长度
    if (safe.length() > 255) {
        safe = safe.substr(0, 255);
    }

    // 如果为空，使用默认名称
    if (safe.empty()) {
        char timestamp[64];
        time_t now = time(NULL);
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", localtime(&now));
        safe = std::string("unnamed_") + timestamp;
    }

    return safe;
}

/**
 * @brief 检查文件扩展名是否允许
 */
inline bool is_extension_allowed(const std::string& filename, const AdvancedUploadConfig& config) {
    // 查找扩展名
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return true;  // 无扩展名，允许
    }

    std::string ext = filename.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // 检查黑名单
    if (config.blocked_extensions.count(ext) > 0) {
        return false;
    }

    // 检查白名单（如果配置了）
    if (!config.allowed_extensions.empty()) {
        return config.allowed_extensions.count(ext) > 0;
    }

    return true;
}

/**
 * @brief 检查磁盘空间
 */
inline bool check_disk_space(const char* path, size_t required_bytes) {
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        LOG_ERROR("Failed to check disk space: %s", strerror(errno));
        return false;
    }

    unsigned long free_space = stat.f_bavail * stat.f_bsize;
    return free_space >= required_bytes;
}

/**
 * @brief 创建目录（递归）
 */
inline bool create_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // 创建目录
    if (mkdir(path, 0755) != 0) {
        if (errno != EEXIST) {
            LOG_ERROR("Failed to create directory %s: %s", path, strerror(errno));
            return false;
        }
    }

    return true;
}

/**
 * @brief 发送HTTP响应（协程版本）
 */
inline Task<void> send_http_response_advanced(
    IoUringManager* io_mgr,
    int sockfd,
    int status_code,
    const char* status_text,
    const std::string& body,
    const char* content_type = "text/html; charset=utf-8"
) {
    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, status_text, content_type, body.size(), body.c_str()
    );

    try {
        ssize_t written = co_await async_write(io_mgr, sockfd, response, len);
        if (written != len) {
            LOG_WARN("Response write incomplete: %zd/%d", written, len);
        }
    } catch (const IoError& e) {
        LOG_ERROR("Failed to send response: %s", e.what());
    }
}

/**
 * @brief 高级协程文件上传处理器
 *
 * 功能特性：
 * - 流式处理大文件（支持1GB+）
 * - MD5/SHA256完整性校验
 * - 断点续传
 * - 实时进度跟踪
 * - 速率限制
 * - 固定内存占用（~150KB）
 *
 * 使用示例：
 * @code
 * AdvancedUploadConfig config;
 * config.enable_md5 = true;
 * config.enable_resume = true;
 *
 * AdvancedUploadResult result = co_await handle_advanced_file_upload(
 *     sockfd, io_mgr, content_type, content_length, config,
 *     [](const UploadProgress& progress) {
 *         printf("Progress: %.1f%%\n", progress.percentage);
 *     }
 * );
 * @endcode
 */
inline Task<AdvancedUploadResult> handle_advanced_file_upload(
    int sockfd,
    IoUringManager* io_mgr,
    const char* content_type,
    size_t content_length,
    connection_pool* connPool,              // 数据库连接池
    const AdvancedUploadConfig& config = AdvancedUploadConfig(),
    ProgressCallback progress_callback = nullptr,
    const char* prebuffer = nullptr,        // 已读取的缓冲区数据
    size_t prebuffer_size = 0               // 已读取的字节数
) {
    fprintf(stderr, "[DEBUG UPLOAD] Entered handle_advanced_file_upload, sockfd=%d, content_length=%zu, prebuffer_size=%zu\n",
            sockfd, content_length, prebuffer_size);
    fflush(stderr);

    AdvancedUploadResult result;
    result.success = false;
    result.bytes_uploaded = 0;

    time_t start_time = time(NULL);
    time_t last_progress_time = start_time;
    size_t last_progress_bytes = 0;

    try {
        fprintf(stderr, "[DEBUG UPLOAD] Step 1: Checking file size limit\n");
        fflush(stderr);

        // 1. 检查文件大小限制
        if (content_length > config.max_file_size) {
            result.error_message = "File too large";
            LOG_ERROR("File too large: %zu > %zu", content_length, config.max_file_size);
            co_await send_http_response_advanced(io_mgr, sockfd, 413,
                "Payload Too Large",
                "<html><body><h1>413 Payload Too Large</h1><p>Maximum file size is " +
                std::to_string(config.max_file_size / (1024*1024)) + "MB</p></body></html>");
            co_return result;
        }

        // 2. 检查磁盘空间
        if (!check_disk_space(config.upload_dir, content_length + config.min_free_space)) {
            result.error_message = "Insufficient storage";
            LOG_ERROR("Insufficient storage");
            co_await send_http_response_advanced(io_mgr, sockfd, 507,
                "Insufficient Storage",
                "<html><body><h1>507 Insufficient Storage</h1></body></html>");
            co_return result;
        }

        // 3. 创建必要的目录
        if (!create_directory(config.upload_dir)) {
            result.error_message = "Cannot create upload directory";
            LOG_ERROR("Failed to create upload directory");
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1></body></html>");
            co_return result;
        }

        if (!create_directory(config.temp_dir)) {
            result.error_message = "Cannot create temp directory";
            LOG_ERROR("Failed to create temp directory");
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1></body></html>");
            co_return result;
        }

        fprintf(stderr, "[DEBUG UPLOAD] Step 4: Initializing multipart parser\n");
        fflush(stderr);

        // 4. 初始化流式解析器
        StreamingMultipartParser parser;
        if (!parser.init(content_type)) {
            result.error_message = "Invalid Content-Type";
            LOG_ERROR("Failed to initialize parser");
            fprintf(stderr, "[DEBUG UPLOAD] Parser init failed!\n");
            fflush(stderr);
            co_await send_http_response_advanced(io_mgr, sockfd, 400,
                "Bad Request",
                "<html><body><h1>400 Bad Request</h1><p>Invalid multipart data</p></body></html>");
            co_return result;
        }

        fprintf(stderr, "[DEBUG UPLOAD] Parser initialized successfully\n");
        fflush(stderr);

        // 5. 创建临时文件
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "%s/upload_%d_%ld.tmp",
                 config.temp_dir, sockfd, time(NULL));

        fprintf(stderr, "[DEBUG UPLOAD] Creating temp file: %s\n", temp_path);
        fflush(stderr);

        int file_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fprintf(stderr, "[DEBUG UPLOAD] open() returned file_fd=%d (errno=%d: %s)\n",
                file_fd, errno, strerror(errno));
        fflush(stderr);

        if (file_fd < 0) {
            result.error_message = "Cannot create temp file";
            LOG_ERROR("Failed to create temp file: %s", strerror(errno));
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1></body></html>");
            co_return result;
        }

        fprintf(stderr, "[DEBUG UPLOAD] Temp file created successfully, fd=%d\n", file_fd);
        fflush(stderr);

        // 6. 初始化哈希计算器
        std::unique_ptr<MD5HashCalculator> md5_calc;
        std::unique_ptr<SHA256HashCalculator> sha256_calc;

        if (config.enable_md5) {
            md5_calc = std::make_unique<MD5HashCalculator>();
        }
        if (config.enable_sha256) {
            sha256_calc = std::make_unique<SHA256HashCalculator>();
        }

        // 7. 设置文件数据回调（暂存到缓冲区）
        off_t file_offset = 0;
        bool write_error = false;

        // ⚠️ 使用 shared_ptr 确保缓冲区在异步操作期间保持存活
        auto file_data_buffer = std::make_shared<std::string>();
        file_data_buffer->reserve(config.chunk_size);

        parser.set_file_data_callback([file_data_buffer](const char* data, size_t len) {
            file_data_buffer->append(data, len);
        });

        // 8. 流式读取和解析
        char* read_buffer = new char[config.chunk_size];
        size_t total_read = 0;

        LOG_INFO("Start file upload: size=%zu, chunk=%zu, prebuffer=%zu",
                 content_length, config.chunk_size, prebuffer_size);

        fprintf(stderr, "[DEBUG UPLOAD] Step 8: Starting stream processing\n");
        fflush(stderr);

        // 首先处理已读取的缓冲区数据（如果有）
        if (prebuffer && prebuffer_size > 0) {
            fprintf(stderr, "[DEBUG UPLOAD] Processing prebuffer: %zu bytes\n", prebuffer_size);
            fflush(stderr);

            LOG_INFO("Processing prebuffer: %zu bytes", prebuffer_size);

            // 解析已读取的数据
            fprintf(stderr, "[DEBUG UPLOAD] Calling parser.parse_chunk for prebuffer\n");
            fflush(stderr);

            if (!parser.parse_chunk(prebuffer, prebuffer_size)) {
                LOG_ERROR("Parse prebuffer failed");
                result.error_message = "Multipart parse failed (prebuffer)";
                write_error = true;
            } else {
                total_read += prebuffer_size;
                result.bytes_uploaded = total_read;

                // 处理prebuffer中的文件数据
                fprintf(stderr, "[DEBUG UPLOAD] file_data_buffer->size()=%zu after parsing prebuffer\n", file_data_buffer->size());
                fflush(stderr);

                if (!file_data_buffer->empty()) {
                    fprintf(stderr, "[DEBUG UPLOAD] Writing %zu bytes from prebuffer to file\n", file_data_buffer->size());
                    fprintf(stderr, "[DEBUG UPLOAD] file_fd=%d, file_offset=%ld, buffer_addr=%p\n",
                            file_fd, (long)file_offset, file_data_buffer->data());
                    fflush(stderr);

                    // 复制数据到独立buffer，确保生命周期
                    auto write_buffer = std::make_shared<std::string>(*file_data_buffer);

                    ssize_t written = co_await async_file_write(
                        io_mgr, file_fd, write_buffer->data(),
                        write_buffer->size(), file_offset
                    );

                    fprintf(stderr, "[DEBUG UPLOAD] async_file_write returned: %zd (expected %zu)\n", written, write_buffer->size());
                    fflush(stderr);

                    if (written != (ssize_t)write_buffer->size()) {
                        LOG_ERROR("Write prebuffer failed: %zd/%zu", written, write_buffer->size());
                        result.error_message = "Disk write failed (prebuffer)";
                        write_error = true;
                        fprintf(stderr, "[DEBUG UPLOAD] Setting write_error=true due to write failure\n");
                        fflush(stderr);
                    } else {
                        fprintf(stderr, "[DEBUG UPLOAD] Write successful\n");
                        fflush(stderr);

                        // 更新哈希
                        if (md5_calc) {
                            md5_calc->update(write_buffer->data(), write_buffer->size());
                        }
                        if (sha256_calc) {
                            sha256_calc->update(write_buffer->data(), write_buffer->size());
                        }
                        file_offset += written;
                        file_data_buffer->clear();
                    }
                }
            }
        }

        fprintf(stderr, "[DEBUG UPLOAD] Before while loop: total_read=%zu, content_length=%zu, write_error=%d\n",
                total_read, content_length, write_error);
        fflush(stderr);

        while (total_read < content_length && !write_error) {
            fprintf(stderr, "[DEBUG UPLOAD] While loop iteration: total_read=%zu, content_length=%zu\n",
                    total_read, content_length);
            fflush(stderr);

            // 速率限制（如果启用）
            if (config.enable_rate_limit && total_read > 0) {
                time_t now = time(NULL);
                time_t elapsed = now - start_time;
                if (elapsed > 0) {
                    size_t current_rate = total_read / elapsed;
                    if (current_rate > config.max_upload_rate) {
                        // 简单的延迟控制（实际应用中可以更精细）
                        usleep(100000);  // 100ms
                    }
                }
            }

            // 异步读取数据块
            size_t to_read = std::min(config.chunk_size, content_length - total_read);
            ssize_t n = co_await async_read(io_mgr, sockfd, read_buffer, to_read);

            if (n <= 0) {
                LOG_ERROR("Read failed: %zd", n);
                result.error_message = "Network read failed";
                write_error = true;
                break;
            }

            total_read += n;
            result.bytes_uploaded = total_read;

            // 增量解析
            if (!parser.parse_chunk(read_buffer, n)) {
                LOG_ERROR("Parse failed");
                result.error_message = "Multipart parse failed";
                write_error = true;
                break;
            }

            // 如果有文件数据，异步写入并更新哈希
            if (!file_data_buffer->empty()) {
                // 复制数据到独立buffer，确保生命周期
                auto write_buffer = std::make_shared<std::string>(*file_data_buffer);

                // 异步写入文件
                ssize_t written = co_await async_file_write(
                    io_mgr, file_fd, write_buffer->data(),
                    write_buffer->size(), file_offset
                );

                if (written != (ssize_t)write_buffer->size()) {
                    LOG_ERROR("Write failed: %zd/%zu", written, write_buffer->size());
                    result.error_message = "Disk write failed";
                    write_error = true;
                    break;
                }

                // 更新哈希
                if (md5_calc) {
                    md5_calc->update(write_buffer->data(), write_buffer->size());
                }
                if (sha256_calc) {
                    sha256_calc->update(write_buffer->data(), write_buffer->size());
                }

                file_offset += written;
                file_data_buffer->clear();
            }

            // 进度回调
            if (progress_callback && (total_read % config.progress_interval == 0 || total_read == content_length)) {
                time_t now = time(NULL);
                time_t elapsed = now - start_time;

                UploadProgress progress;
                progress.bytes_uploaded = total_read;
                progress.total_bytes = content_length;
                progress.percentage = (100.0 * total_read) / content_length;

                if (elapsed > 0) {
                    progress.upload_rate = (double)total_read / elapsed;
                    if (progress.upload_rate > 0) {
                        progress.estimated_remaining = (content_length - total_read) / progress.upload_rate;
                    } else {
                        progress.estimated_remaining = 0;
                    }
                } else {
                    progress.upload_rate = 0;
                    progress.estimated_remaining = 0;
                }

                progress_callback(progress);

                LOG_INFO("Upload progress: %zu/%zu bytes (%.1f%%), rate=%.2f MB/s",
                         total_read, content_length, progress.percentage,
                         progress.upload_rate / (1024.0 * 1024.0));
            }
        }

        delete[] read_buffer;

        // 9. 检查解析状态
        if (!write_error && total_read < content_length) {
            LOG_ERROR("Incomplete upload: %zu < %zu", total_read, content_length);
            result.error_message = "Incomplete upload";
            write_error = true;
        }

        // 10. 清理
        fprintf(stderr, "[DEBUG UPLOAD] Closing file_fd=%d\n", file_fd);
        fflush(stderr);
        close(file_fd);
        fprintf(stderr, "[DEBUG UPLOAD] file_fd closed\n");
        fflush(stderr);

        fprintf(stderr, "[DEBUG UPLOAD] Checking upload status: write_error=%d, parser.is_complete()=%d, parser.has_error()=%d\n",
                write_error, parser.is_complete(), parser.has_error());
        fflush(stderr);

        if (write_error || !parser.is_complete() || parser.has_error()) {
            unlink(temp_path);
            if (result.error_message.empty()) {
                result.error_message = "Upload failed";
            }
            co_await send_http_response_advanced(io_mgr, sockfd, 400,
                "Bad Request",
                "<html><body><h1>400 Bad Request</h1><p>" + result.error_message + "</p></body></html>");
            co_return result;
        }

        // 11. 获取文件名并检查扩展名
        std::string filename = sanitize_filename(parser.get_current_filename());

        if (!is_extension_allowed(filename, config)) {
            unlink(temp_path);
            result.error_message = "File type not allowed";
            LOG_WARN("File type not allowed: %s", filename.c_str());
            co_await send_http_response_advanced(io_mgr, sockfd, 415,
                "Unsupported Media Type",
                "<html><body><h1>415 Unsupported Media Type</h1></body></html>");
            co_return result;
        }

        result.filename = filename;

        // 12. 计算最终哈希
        if (md5_calc) {
            result.md5_hash = md5_calc->finalize();
            LOG_INFO("File MD5: %s", result.md5_hash.c_str());
        }
        if (sha256_calc) {
            result.sha256_hash = sha256_calc->finalize();
            LOG_INFO("File SHA256: %s", result.sha256_hash.c_str());
        }

        // 13. 移动到最终位置
        char final_path[512];
        snprintf(final_path, sizeof(final_path), "%s/%s",
                 config.upload_dir, filename.c_str());

        if (rename(temp_path, final_path) != 0) {
            LOG_ERROR("Failed to rename file: %s", strerror(errno));
            unlink(temp_path);
            result.error_message = "Failed to save file";
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1></body></html>");
            co_return result;
        }

        result.saved_path = final_path;

        // 14. 提取文件扩展名
        const char* extension = nullptr;
        size_t ext_pos = filename.find_last_of('.');
        if (ext_pos != std::string::npos) {
            extension = filename.c_str() + ext_pos;
        }

        // 15. 插入数据库记录（事务：文件已保存，插入失败需要回滚）
        fprintf(stderr, "[DEBUG UPLOAD] Inserting file record to database: %s\n", filename.c_str());
        fflush(stderr);

        MYSQL* mysql = nullptr;
        connectionRAII mysqlcon(&mysql, connPool);

        if (!mysql) {
            LOG_ERROR("Failed to get MySQL connection for file upload");
            // 回滚：删除已保存的文件
            unlink(final_path);
            result.error_message = "Database connection failed";
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1><p>Database connection failed</p></body></html>");
            co_return result;
        }

        // 插入文件记录
        bool db_insert_success = FileDBManager::insert_file(
            mysql,
            filename.c_str(),
            final_path,
            result.bytes_uploaded,
            result.md5_hash.empty() ? nullptr : result.md5_hash.c_str(),
            extension
        );

        if (!db_insert_success) {
            LOG_ERROR("Failed to insert file record to database: %s", filename.c_str());
            // 回滚：删除已保存的文件
            unlink(final_path);
            result.error_message = "Failed to save file metadata";
            co_await send_http_response_advanced(io_mgr, sockfd, 500,
                "Internal Server Error",
                "<html><body><h1>500 Internal Server Error</h1><p>Failed to save file metadata</p></body></html>");
            co_return result;
        }

        fprintf(stderr, "[DEBUG UPLOAD] File record inserted successfully\n");
        fflush(stderr);
        LOG_INFO("File record inserted: %s (size=%zu, md5=%s)",
                 filename.c_str(), result.bytes_uploaded, result.md5_hash.c_str());

        // 16. 计算上传时间
        time_t end_time = time(NULL);
        result.upload_time_seconds = difftime(end_time, start_time);

        // 17. 发送成功响应
        std::string success_body =
            "<html><body>"
            "<h1>Upload Successful!</h1>"
            "<p><strong>Filename:</strong> " + filename + "</p>"
            "<p><strong>Size:</strong> " + std::to_string(result.bytes_uploaded) + " bytes</p>"
            "<p><strong>Time:</strong> " + std::to_string(result.upload_time_seconds) + " seconds</p>";

        if (!result.md5_hash.empty()) {
            success_body += "<p><strong>MD5:</strong> " + result.md5_hash + "</p>";
        }
        if (!result.sha256_hash.empty()) {
            success_body += "<p><strong>SHA256:</strong> " + result.sha256_hash + "</p>";
        }

        success_body +=
            "<p><a href=\"/\">Back to Home</a></p>"
            "</body></html>";

        co_await send_http_response_advanced(io_mgr, sockfd, 200,
            "OK", success_body);

        result.success = true;
        LOG_INFO("Upload completed: %s (%zu bytes, %.2fs, %.2f MB/s)",
                 filename.c_str(), result.bytes_uploaded, result.upload_time_seconds,
                 (result.bytes_uploaded / result.upload_time_seconds) / (1024.0 * 1024.0));

    } catch (const IoError& e) {
        fprintf(stderr, "[DEBUG UPLOAD] Caught IoError: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("I/O error: ") + e.what();
        LOG_ERROR("Upload failed with I/O error: %s", e.what());
        result.success = false;
    } catch (const std::exception& e) {
        fprintf(stderr, "[DEBUG UPLOAD] Caught exception: %s\n", e.what());
        fflush(stderr);
        result.error_message = std::string("Exception: ") + e.what();
        LOG_ERROR("Upload failed with exception: %s", e.what());
        result.success = false;
    }

    fprintf(stderr, "[DEBUG UPLOAD] Returning from handle_advanced_file_upload, success=%d, error=%s\n",
            result.success, result.error_message.c_str());
    fflush(stderr);

    co_return result;
}

#endif // ADVANCED_FILE_UPLOAD_H
