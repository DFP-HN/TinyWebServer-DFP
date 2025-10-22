/**
 * @file example_http.cpp
 * @brief 协程版本 HTTP 处理示例
 *
 * 展示如何使用协程处理 HTTP 连接
 */

#include "task.h"
#include "io_awaiter.h"
#include "scheduler.h"
#include "../http/http_conn.h"
#include "../log/log.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

/**
 * @brief 协程版本：处理单个 HTTP 连接
 *
 * 相比回调版本，代码更清晰、更易维护
 */
Task<void> handle_http_connection_coro(
    int sockfd,
    http_conn* conn,
    IoUringManager* io_mgr
)
{
    char buffer[http_conn::READ_BUFFER_SIZE];

    try {
        LOG_INFO("Starting HTTP connection handler: fd=%d", sockfd);

        while (true) {
            // ==================== 1. 异步读取 HTTP 请求 ====================
            LOG_DEBUG("Waiting for HTTP request: fd=%d", sockfd);

            ssize_t n = co_await async_read(
                io_mgr, sockfd, buffer, sizeof(buffer)
            );

            if (n == 0) {
                // 连接关闭
                LOG_INFO("Connection closed by peer: fd=%d", sockfd);
                break;
            }

            LOG_DEBUG("Received %ld bytes from fd=%d", n, sockfd);

            // ==================== 2. 解析 HTTP 请求 ====================
            conn->append_read_buffer(buffer, n);
            auto http_code = conn->process_read();

            if (http_code == http_conn::NO_REQUEST) {
                // 请求不完整，继续读取
                LOG_DEBUG("Incomplete HTTP request, continue reading");
                continue;
            }

            if (http_code == http_conn::BAD_REQUEST) {
                LOG_WARN("Bad HTTP request from fd=%d", sockfd);
                // 发送 400 错误响应
                const char* bad_request_response =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";

                co_await async_write(
                    io_mgr, sockfd,
                    bad_request_response,
                    strlen(bad_request_response)
                );
                break;
            }

            // ==================== 3. 生成响应 ====================
            if (!conn->process_write(http_code)) {
                LOG_ERROR("Failed to generate HTTP response");
                break;
            }

            // ==================== 4. 异步发送响应 ====================

            // 4.1 发送响应头
            struct iovec* iv = conn->get_iovec();
            int iv_count = conn->get_iovec_count();

            if (iv_count > 0 && iv[0].iov_len > 0) {
                ssize_t sent = co_await async_write(
                    io_mgr, sockfd,
                    iv[0].iov_base,
                    iv[0].iov_len
                );
                LOG_DEBUG("Sent response header: %ld bytes", sent);
            }

            // 4.2 发送响应体（如果有）
            if (conn->is_using_sendfile()) {
                // 发送文件内容（分块传输）
                int file_fd = conn->get_file_fd();
                size_t file_size = conn->get_file_size();

                const size_t CHUNK_SIZE = 128 * 1024;  // 128KB
                char file_buffer[CHUNK_SIZE];

                LOG_DEBUG("Sending file: size=%zu bytes", file_size);

                for (size_t offset = 0; offset < file_size; offset += CHUNK_SIZE) {
                    size_t chunk_size = std::min(CHUNK_SIZE, file_size - offset);

                    // 读取文件块（同步，本地文件很快）
                    ssize_t read_bytes = pread(file_fd, file_buffer, chunk_size, offset);
                    if (read_bytes <= 0) {
                        throw IoError("pread failed");
                    }

                    // 异步发送文件块
                    ssize_t sent = co_await async_write(
                        io_mgr, sockfd,
                        file_buffer,
                        read_bytes
                    );

                    LOG_DEBUG("Sent file chunk: %ld/%zu bytes (offset=%zu)",
                              sent, file_size, offset);
                }

                LOG_INFO("File transfer completed: %zu bytes", file_size);
            }
            else if (iv_count > 1 && iv[1].iov_len > 0) {
                // 使用 mmap 方式，直接发送
                ssize_t sent = co_await async_write(
                    io_mgr, sockfd,
                    iv[1].iov_base,
                    iv[1].iov_len
                );
                LOG_DEBUG("Sent response body: %ld bytes", sent);
            }

            // ==================== 5. 检查是否保持连接 ====================
            if (!conn->get_linger()) {
                LOG_DEBUG("Connection will close (no keep-alive)");
                break;
            }

            // 重置连接，准备下一个请求
            LOG_DEBUG("Keep-alive, waiting for next request");
            conn->reset_connection();
        }
    }
    catch (const IoError& e) {
        LOG_ERROR("I/O error on fd=%d: %s (errno=%d)",
                  sockfd, e.what(), e.error_code);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception on fd=%d: %s", sockfd, e.what());
    }

    // ==================== 6. 关闭连接 ====================
    close(sockfd);
    LOG_INFO("Connection closed: fd=%d", sockfd);
}

/**
 * @brief 协程版本：Accept 新连接
 */
Task<void> accept_connections_coro(
    int listen_fd,
    http_conn* users,
    IoUringManager* io_mgr,
    CoroScheduler* scheduler
)
{
    struct sockaddr_in client_addr;
    socklen_t client_addrlen;

    try {
        while (true) {
            // 异步 accept
            client_addrlen = sizeof(client_addr);
            int connfd = co_await async_accept(
                io_mgr, listen_fd,
                (struct sockaddr*)&client_addr,
                &client_addrlen
            );

            if (connfd < 0) {
                LOG_ERROR("accept failed: %d", connfd);
                continue;
            }

            LOG_INFO("New connection accepted: fd=%d, ip=%s",
                     connfd, inet_ntoa(client_addr.sin_addr));

            // 初始化连接
            http_conn* conn = &users[connfd];
            conn->init(connfd, client_addr, /* ... 其他参数 ... */);

            // 启动处理该连接的协程
            scheduler->spawn(
                handle_http_connection_coro(connfd, conn, io_mgr)
            );

            LOG_DEBUG("Spawned new coroutine for fd=%d", connfd);
        }
    }
    catch (const IoError& e) {
        LOG_ERROR("Accept loop error: %s", e.what());
    }
}

/**
 * @brief 示例：简单的 echo 服务器（协程版本）
 */
Task<void> echo_server_coro(int sockfd, IoUringManager* io_mgr)
{
    char buffer[1024];

    try {
        while (true) {
            // 读取数据
            ssize_t n = co_await async_read(io_mgr, sockfd, buffer, sizeof(buffer));

            if (n == 0) {
                break;  // 连接关闭
            }

            LOG_INFO("Echo: received %ld bytes", n);

            // 回显数据
            co_await async_write(io_mgr, sockfd, buffer, n);
        }
    }
    catch (const IoError& e) {
        LOG_ERROR("Echo error: %s", e.what());
    }

    close(sockfd);
}

/**
 * @brief 主函数示例
 */
/*
int main() {
    // 1. 初始化 io_uring
    IoUringManager io_mgr(256);
    io_mgr.init();

    // 2. 创建调度器
    CoroScheduler scheduler(&io_mgr);

    // 3. 启动 accept 协程
    int listen_fd = socket(...);
    bind(listen_fd, ...);
    listen(listen_fd, ...);

    http_conn users[MAX_FD];

    scheduler.spawn(
        accept_connections_coro(listen_fd, users, &io_mgr, &scheduler)
    );

    // 4. 运行事件循环
    scheduler.run();

    return 0;
}
*/
