#ifndef THREAD_POOL_BRIDGE_H
#define THREAD_POOL_BRIDGE_H

#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <sys/eventfd.h>
#include <unistd.h>
#include <coroutine>
#include "task.h"
#include "io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../threadpool/cpu_thread_pool.h"
#include "../log/log.h"

/**
 * @brief CPU任务结果包装器
 */
template<typename T>
struct CpuTaskResult {
    bool success = false;
    T value;
    std::string error_message;

    CpuTaskResult() = default;

    explicit CpuTaskResult(T val) : success(true), value(std::move(val)) {}

    static CpuTaskResult make_error(const char* msg) {
        CpuTaskResult result;
        result.success = false;
        result.error_message = msg;
        return result;
    }
};

/**
 * @brief CPU任务Awaiter（用于co_await）
 *
 * 工作流程：
 * 1. 协程调用 co_await run_in_threadpool(callable)
 * 2. CpuTaskAwaiter 将任务提交到线程池
 * 3. 线程池执行完成后，写入eventfd通知io_uring
 * 4. io_uring 检测到eventfd可读，恢复协程
 * 5. 协程继续执行，获取CPU任务结果
 */
template<typename T>
class CpuTaskAwaiter {
public:
    CpuTaskAwaiter(std::function<T()> callable, int eventfd, IoUringManager* io_mgr)
        : task_callable(std::move(callable)),
          event_fd(eventfd),
          io_uring_mgr(io_mgr),
          result_ready(false),
          coroutine_handle(nullptr) {
    }

    // 1. await_ready: 检查结果是否已就绪（始终返回false，需要挂起）
    bool await_ready() const noexcept {
        return false;  // CPU任务总是异步的
    }

    // 2. await_suspend: 挂起协程，提交任务到线程池
    void await_suspend(std::coroutine_handle<> handle) {
        coroutine_handle = handle;

        LOG_DEBUG("CpuTaskAwaiter: Suspending coroutine, submitting to thread pool");

        // 任务完成回调（在线程池线程中执行）
        auto completion_callback = [this, handle]() {
            try {
                // 执行CPU任务
                LOG_DEBUG("Thread pool: Executing CPU task");
                T value = this->task_callable();

                // 保存结果
                {
                    std::lock_guard<std::mutex> lock(this->result_mutex);
                    this->result = CpuTaskResult<T>(std::move(value));
                    this->result_ready = true;
                }

                LOG_DEBUG("Thread pool: Task completed, notifying io_uring via eventfd");

                // 通知io_uring（写入eventfd）
                uint64_t notify = 1;
                ssize_t n = write(this->event_fd, &notify, sizeof(notify));
                if (n != sizeof(notify)) {
                    LOG_ERROR("Failed to write eventfd: %s", strerror(errno));
                }

            } catch (const std::exception& e) {
                LOG_ERROR("CPU task exception: %s", e.what());

                // 保存错误
                {
                    std::lock_guard<std::mutex> lock(this->result_mutex);
                    this->result = CpuTaskResult<T>::make_error(e.what());
                    this->result_ready = true;
                }

                // 仍然需要通知io_uring
                uint64_t notify = 1;
                write(this->event_fd, &notify, sizeof(notify));
            }
        };

        // 提交到线程池（这里需要适配不同的线程池接口）
        // 注意：这里简化处理，实际需要根据线程池API调整
        submit_to_thread_pool(std::move(completion_callback));

        // 提交io_uring读操作，监听eventfd
        submit_eventfd_read();
    }

    // 3. await_resume: 协程恢复时调用，返回CPU任务结果
    T await_resume() {
        LOG_DEBUG("CpuTaskAwaiter: Resuming coroutine, returning result");

        std::lock_guard<std::mutex> lock(result_mutex);

        if (!result_ready) {
            throw std::runtime_error("CPU task result not ready");
        }

        if (!result.success) {
            throw std::runtime_error("CPU task failed: " + result.error_message);
        }

        return std::move(result.value);
    }

private:
    // 提交到线程池
    void submit_to_thread_pool(std::function<void()> callback) {
        // 使用全局CPU线程池（直接调用，不需要extern声明）
        bool success = get_global_cpu_thread_pool().enqueue(std::move(callback));

        if (!success) {
            LOG_ERROR("Failed to enqueue CPU task: thread pool queue full");
            // 设置错误结果
            {
                std::lock_guard<std::mutex> lock(this->result_mutex);
                this->result = CpuTaskResult<T>::make_error("Thread pool queue full");
                this->result_ready = true;
            }
            // 仍然需要通知io_uring
            uint64_t notify = 1;
            write(this->event_fd, &notify, sizeof(notify));
        }
    }

    // 提交eventfd读操作到io_uring
    void submit_eventfd_read() {
        // 准备读缓冲区
        eventfd_buffer = std::make_shared<uint64_t>(0);

        // 准备io_uring读操作（使用自定义Awaiter）
        // 注意：这里需要特殊处理，因为我们在await_suspend中，不能再co_await
        // 解决方案：直接使用io_uring_manager的底层API

        struct io_uring_sqe* sqe = io_uring_mgr->get_sqe();
        if (!sqe) {
            LOG_ERROR("Failed to get SQE for eventfd read");
            return;
        }

        io_uring_prep_read(sqe, event_fd, eventfd_buffer.get(), sizeof(uint64_t), 0);
        io_uring_sqe_set_data(sqe, this);  // user_data指向自己

        LOG_DEBUG("CpuTaskAwaiter: Submitted eventfd read to io_uring");
    }

public:
    // io_uring完成时的回调（由调度器调用）
    void on_eventfd_ready(int res) {
        LOG_DEBUG("CpuTaskAwaiter: eventfd ready, res=%d", res);

        if (res < 0) {
            LOG_ERROR("eventfd read failed: %d", res);

            // 设置错误结果
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                result = CpuTaskResult<T>::make_error("eventfd read failed");
                result_ready = true;
            }
        }

        // 恢复协程
        if (coroutine_handle) {
            LOG_DEBUG("CpuTaskAwaiter: Resuming coroutine handle");
            coroutine_handle.resume();
        }
    }

private:
    std::function<T()> task_callable;
    int event_fd;
    IoUringManager* io_uring_mgr;

    CpuTaskResult<T> result;
    std::mutex result_mutex;
    bool result_ready;

    std::coroutine_handle<> coroutine_handle;
    std::shared_ptr<uint64_t> eventfd_buffer;  // 保持缓冲区生命周期
};

/**
 * @brief 线程池桥接管理器
 *
 * 管理eventfd和Awaiter的生命周期
 */
class ThreadPoolBridge {
public:
    ThreadPoolBridge() {
        // 创建eventfd用于线程池->io_uring通信
        event_fd = eventfd(0, EFD_NONBLOCK);
        if (event_fd < 0) {
            throw std::runtime_error("Failed to create eventfd");
        }
        LOG_INFO("ThreadPoolBridge created (eventfd=%d)", event_fd);
    }

    ~ThreadPoolBridge() {
        if (event_fd >= 0) {
            close(event_fd);
        }
    }

    /**
     * @brief 在线程池中运行CPU任务（协程版本）
     *
     * 使用示例：
     * @code
     * Task<void> my_coroutine() {
     *     auto result = co_await bridge.run_in_threadpool<int>([]() {
     *         return compute_primes(1000);
     *     }, io_mgr);
     *     LOG_INFO("Prime result: %d", result);
     * }
     * @endcode
     */
    template<typename T>
    auto run_in_threadpool(std::function<T()> callable, IoUringManager* io_mgr) {
        return CpuTaskAwaiter<T>(std::move(callable), event_fd, io_mgr);
    }

    int get_eventfd() const { return event_fd; }

private:
    int event_fd;
};

/**
 * @brief 全局线程池桥接实例（单例）
 *
 * 使用单例模式避免多次创建eventfd
 */
inline ThreadPoolBridge& get_global_thread_pool_bridge() {
    static ThreadPoolBridge bridge;
    return bridge;
}

/**
 * @brief 便捷函数：在线程池中运行CPU任务
 *
 * 使用示例：
 * @code
 * Task<void> handle_request() {
 *     // I/O操作
 *     auto request = co_await async_read(...);
 *
 *     // CPU密集计算（在线程池执行）
 *     auto hash = co_await run_in_threadpool<std::string>([&]() {
 *         return compute_hash(request.data);
 *     }, io_mgr);
 *
 *     // 继续I/O操作
 *     co_await async_write(..., hash);
 * }
 * @endcode
 */
template<typename T>
inline auto run_in_threadpool(std::function<T()> callable, IoUringManager* io_mgr) {
    return get_global_thread_pool_bridge().run_in_threadpool<T>(
        std::move(callable), io_mgr);
}

#endif // THREAD_POOL_BRIDGE_H
