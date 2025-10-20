#include "zero_copy.h"
#include "../log/log.h"
#include <string.h>

// 静态成员初始化
int ZeroCopyManager::s_pipefd[2] = {-1, -1};
bool ZeroCopyManager::s_pipe_initialized = false;

ZeroCopyManager::TransferMethod ZeroCopyManager::choose_method(size_t file_size, bool is_cached) {
    // 如果文件已经缓存在内存中，直接使用缓存
    if (is_cached) {
        return USE_CACHE;
    }

    // 根据文件大小选择传输方式
    if (file_size < SMALL_FILE_THRESHOLD) {
        // 小文件：优先使用缓存，否则使用普通读写
        return USE_CACHE;
    } else if (file_size < LARGE_FILE_THRESHOLD) {
        // 中等文件：使用 sendfile 零拷贝
        return USE_SENDFILE;
    } else {
        // 大文件：使用 mmap 内存映射 + 分块传输
        return USE_MMAP;
    }
}

ssize_t ZeroCopyManager::send_file(int sockfd, int filefd, off_t *offset, size_t count) {
    if (sockfd < 0 || filefd < 0) {
        LOG_ERROR("Invalid file descriptors: sockfd=%d, filefd=%d", sockfd, filefd);
        errno = EBADF;
        return -1;
    }

    ssize_t total_sent = 0;
    ssize_t ret;

    // 循环发送，处理部分发送的情况
    while (count > 0) {
        ret = sendfile(sockfd, filefd, offset, count);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞 socket，缓冲区满，需要稍后重试
                LOG_DEBUG("sendfile: EAGAIN, sent=%ld bytes so far", total_sent);
                break;  // 返回已发送的字节数，上层会重新调度
            } else if (errno == EINTR) {
                // 被信号中断，继续重试
                continue;
            } else {
                // 其他错误
                LOG_ERROR("sendfile failed: %s (sent=%ld bytes)", strerror(errno), total_sent);
                return (total_sent > 0) ? total_sent : -1;
            }
        }

        if (ret == 0) {
            // 文件已发送完毕
            break;
        }

        total_sent += ret;
        count -= ret;
    }

    LOG_DEBUG("sendfile: sent %ld bytes", total_sent);
    return total_sent;
}

bool ZeroCopyManager::init_pipe() {
    if (s_pipe_initialized) {
        return true;
    }

    // 创建管道
    if (pipe(s_pipefd) < 0) {
        LOG_ERROR("Failed to create pipe for splice: %s", strerror(errno));
        return false;
    }

    // 设置管道为非阻塞（可选，根据需求）
    // fcntl(s_pipefd[0], F_SETFL, O_NONBLOCK);
    // fcntl(s_pipefd[1], F_SETFL, O_NONBLOCK);

    s_pipe_initialized = true;
    LOG_INFO("Splice pipe initialized: read_fd=%d, write_fd=%d", s_pipefd[0], s_pipefd[1]);
    return true;
}

void ZeroCopyManager::cleanup_pipe() {
    if (s_pipe_initialized) {
        if (s_pipefd[0] >= 0) {
            close(s_pipefd[0]);
            s_pipefd[0] = -1;
        }
        if (s_pipefd[1] >= 0) {
            close(s_pipefd[1]);
            s_pipefd[1] = -1;
        }
        s_pipe_initialized = false;
        LOG_INFO("Splice pipe cleaned up");
    }
}

ssize_t ZeroCopyManager::splice_transfer(int in_fd, int out_fd, size_t len) {
    if (!init_pipe()) {
        return -1;
    }

    ssize_t total_transferred = 0;
    size_t remaining = len;

    while (remaining > 0) {
        // 步骤 1: 从输入 fd 读取到管道（零拷贝）
        ssize_t ret_in = splice(in_fd, NULL, s_pipefd[1], NULL,
                                 remaining, SPLICE_F_MOVE | SPLICE_F_MORE);

        if (ret_in < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_DEBUG("splice (in): EAGAIN, transferred=%ld bytes so far", total_transferred);
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("splice (in) failed: %s", strerror(errno));
                return (total_transferred > 0) ? total_transferred : -1;
            }
        }

        if (ret_in == 0) {
            // 输入结束
            break;
        }

        // 步骤 2: 从管道写入到输出 fd（零拷贝）
        ssize_t ret_out = splice(s_pipefd[0], NULL, out_fd, NULL,
                                  ret_in, SPLICE_F_MOVE | SPLICE_F_MORE);

        if (ret_out < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_DEBUG("splice (out): EAGAIN");
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("splice (out) failed: %s", strerror(errno));
                return (total_transferred > 0) ? total_transferred : -1;
            }
        }

        total_transferred += ret_out;
        remaining -= ret_out;

        if (ret_out < ret_in) {
            // 管道中还有数据未发送完，需要处理
            LOG_WARN("splice: pipe has %ld bytes remaining", ret_in - ret_out);
            break;
        }
    }

    LOG_DEBUG("splice: transferred %ld bytes", total_transferred);
    return total_transferred;
}

bool ZeroCopyManager::send_response(http_conn *conn) {
    if (!conn) {
        LOG_ERROR("ZeroCopyManager::send_response: null connection");
        return false;
    }

    // 委托给 http_conn 的 write() 方法
    // 这里的逻辑主要是策略选择，实际发送由 http_conn 完成
    return conn->write();
}
