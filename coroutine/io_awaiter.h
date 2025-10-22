#ifndef COROUTINE_IO_AWAITER_H
#define COROUTINE_IO_AWAITER_H

#include <coroutine>
#include <stdexcept>
#include <cstring>
#include "../io_uring/io_uring_manager.h"

/**
 * @brief I/O 错误异常
 */
class IoError : public std::runtime_error {
public:
    IoError(const char* msg, int err = 0)
        : std::runtime_error(msg), error_code(err) {}

    int error_code;
};

/**
 * @brief Awaiter 基类（用于类型安全）
 *
 * 所有 I/O Awaiter 都应该继承此基类，以便在调度器中统一处理
 */
class BaseAwaiter {
public:
    virtual ~BaseAwaiter() = default;
    virtual void complete(int result) = 0;
};

/**
 * @brief io_uring 异步读操作的 Awaiter
 *
 * 用于协程中进行异步读取
 *
 * 使用示例：
 * @code
 * Task<void> read_example() {
 *     char buffer[1024];
 *     ssize_t n = co_await async_read(io_mgr, sockfd, buffer, sizeof(buffer));
 *     // 读取完成，n 是读取的字节数
 * }
 * @endcode
 */
class IoUringReadAwaiter : public BaseAwaiter {
public:
    IoUringReadAwaiter(IoUringManager* mgr, int fd, void* buf, size_t len, off_t off = -1)
        : manager(mgr), sockfd(fd), buffer(buf), length(len), offset(off) {}

    /**
     * @brief 检查是否需要挂起（总是需要，因为是异步 I/O）
     */
    bool await_ready() const noexcept {
        return false;
    }

    /**
     * @brief 协程挂起时调用：提交异步读请求
     */
    void await_suspend(std::coroutine_handle<> handle) {
        coro_handle = handle;

        // 提交异步读请求，user_data 携带 this 指针
        uint64_t user_data = reinterpret_cast<uint64_t>(this);
        bool submitted = manager->submit_read(sockfd, buffer, length, offset, user_data);

        if (!submitted) {
            // 提交失败，立即恢复协程并抛出异常
            result = -EAGAIN;
            handle.resume();
        }
    }

    /**
     * @brief 协程恢复时调用：返回读取结果
     * @return 读取的字节数
     * @throws IoError 如果读取失败
     */
    ssize_t await_resume() {
        if (result < 0) {
            throw IoError("async read failed", -result);
        }
        return result;
    }

    /**
     * @brief 从 io_uring 完成处理中调用，设置结果并恢复协程
     */
    void complete(int res) {
        result = res;
        if (coro_handle) {
            coro_handle.resume();
        }
    }

private:
    IoUringManager* manager;
    int sockfd;
    void* buffer;
    size_t length;
    off_t offset;
    ssize_t result = 0;
    std::coroutine_handle<> coro_handle;
};

/**
 * @brief io_uring 异步写操作的 Awaiter
 */
class IoUringWriteAwaiter : public BaseAwaiter {
public:
    IoUringWriteAwaiter(IoUringManager* mgr, int fd, const void* buf, size_t len, off_t off = -1)
        : manager(mgr), sockfd(fd), buffer(buf), length(len), offset(off) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        coro_handle = handle;

        uint64_t user_data = reinterpret_cast<uint64_t>(this);
        bool submitted = manager->submit_write(sockfd, buffer, length, offset, user_data);

        if (!submitted) {
            result = -EAGAIN;
            handle.resume();
        }
    }

    ssize_t await_resume() {
        if (result < 0) {
            throw IoError("async write failed", -result);
        }
        return result;
    }

    void complete(int res) {
        result = res;
        if (coro_handle) {
            coro_handle.resume();
        }
    }

private:
    IoUringManager* manager;
    int sockfd;
    const void* buffer;
    size_t length;
    off_t offset;
    ssize_t result = 0;
    std::coroutine_handle<> coro_handle;
};

/**
 * @brief io_uring 异步 accept 操作的 Awaiter
 */
class IoUringAcceptAwaiter : public BaseAwaiter {
public:
    IoUringAcceptAwaiter(IoUringManager* mgr, int listen_fd,
                          struct sockaddr* addr, socklen_t* addrlen)
        : manager(mgr), listenfd(listen_fd), client_addr(addr), client_addrlen(addrlen) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_suspend: START, listenfd=%d\n", listenfd);
        fflush(stderr);
        coro_handle = handle;

        uint64_t user_data = reinterpret_cast<uint64_t>(this);
        fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_suspend: Submitting accept, user_data=%p\n", (void*)user_data);
        fflush(stderr);
        bool submitted = manager->submit_accept(listenfd, client_addr, client_addrlen, user_data);
        fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_suspend: submit_accept returned %d\n", submitted);
        fflush(stderr);

        if (!submitted) {
            fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_suspend: Submit failed, resuming immediately\n");
            fflush(stderr);
            result = -EAGAIN;
            handle.resume();
        } else {
            fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_suspend: Submit OK, suspending coroutine\n");
            fflush(stderr);
        }
    }

    int await_resume() {
        fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::await_resume: result=%d\n", result);
        fflush(stderr);
        if (result < 0) {
            throw IoError("async accept failed", -result);
        }
        return result;  // 返回新连接的 fd
    }

    void complete(int res) {
        fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::complete: res=%d, coro_handle=%p\n", res, coro_handle.address());
        fflush(stderr);
        result = res;
        if (coro_handle) {
            fprintf(stderr, "[DEBUG] IoUringAcceptAwaiter::complete: Resuming coroutine\n");
            fflush(stderr);
            coro_handle.resume();
        }
    }

private:
    IoUringManager* manager;
    int listenfd;
    struct sockaddr* client_addr;
    socklen_t* client_addrlen;
    int result = 0;
    std::coroutine_handle<> coro_handle;
};

// ==================== 辅助函数 ====================

/**
 * @brief 异步读取数据
 */
inline IoUringReadAwaiter async_read(
    IoUringManager* mgr,
    int fd,
    void* buffer,
    size_t length,
    off_t offset = -1
)
{
    return IoUringReadAwaiter(mgr, fd, buffer, length, offset);
}

/**
 * @brief 异步写入数据
 */
inline IoUringWriteAwaiter async_write(
    IoUringManager* mgr,
    int fd,
    const void* buffer,
    size_t length,
    off_t offset = -1
)
{
    return IoUringWriteAwaiter(mgr, fd, buffer, length, offset);
}

/**
 * @brief 异步接受连接
 */
inline IoUringAcceptAwaiter async_accept(
    IoUringManager* mgr,
    int listen_fd,
    struct sockaddr* addr,
    socklen_t* addrlen
)
{
    return IoUringAcceptAwaiter(mgr, listen_fd, addr, addrlen);
}

#endif // COROUTINE_IO_AWAITER_H
