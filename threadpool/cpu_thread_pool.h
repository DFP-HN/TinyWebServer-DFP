#ifndef CPU_THREAD_POOL_H
#define CPU_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <memory>
#include "../log/log.h"

/**
 * @brief CPU密集任务线程池
 *
 * 特点：
 * - 专门用于CPU密集型计算（不处理I/O）
 * - 支持任意可调用对象（std::function）
 * - FIFO队列调度（简单高效）
 * - 线程数可配置（默认=CPU核心数）
 * - 任务队列大小限制（防止内存溢出）
 *
 * 使用示例：
 * @code
 * CpuThreadPool pool(8, 1000);  // 8线程，最多1000个待处理任务
 * pool.enqueue([]() {
 *     // CPU密集计算
 *     return compute_primes(1000);
 * });
 * @endcode
 */
class CpuThreadPool {
public:
    /**
     * @brief 构造函数
     * @param num_threads 线程数（0=自动检测CPU核心数）
     * @param max_queue_size 最大队列长度（0=无限制）
     */
    explicit CpuThreadPool(size_t num_threads = 0, size_t max_queue_size = 10000);


    /**
     * @brief 析构函数 - 等待所有任务完成并关闭线程池
     */
    ~CpuThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            shutdown_ = true;
        }
        condition_.notify_all();

        // 等待所有线程结束
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        LOG_INFO("CPU thread pool destroyed (processed %zu tasks)", tasks_processed_.load());
    }

    /**
     * @brief 提交任务到线程池
     * @param task 要执行的任务（返回void的可调用对象）
     * @return true=成功, false=队列已满
     */
    bool enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // 检查队列大小限制
            if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
                LOG_WARN("CPU thread pool queue full (%zu/%zu)",
                         tasks_.size(), max_queue_size_);
                return false;
            }

            if (shutdown_) {
                LOG_WARN("Cannot enqueue: thread pool is shutting down");
                return false;
            }

            tasks_.push(std::move(task));
        }

        condition_.notify_one();
        return true;
    }

    /**
     * @brief 获取当前队列长度
     */
    size_t queue_size() const {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    /**
     * @brief 获取已处理的任务总数
     */
    size_t tasks_processed() const {
        return tasks_processed_.load();
    }

    /**
     * @brief 获取线程数
     */
    size_t thread_count() const {
        return workers_.size();
    }

    /**
     * @brief 打印统计信息
     */
    void print_stats() const {
        LOG_INFO("=== CPU Thread Pool Stats ===");
        LOG_INFO("Threads: %zu", thread_count());
        LOG_INFO("Queue size: %zu/%zu", queue_size(), max_queue_size_);
        LOG_INFO("Tasks processed: %zu", tasks_processed());
    }

private:
    /**
     * @brief 工作线程主循环
     */
    void worker_thread(size_t thread_id) {
        LOG_DEBUG("CPU worker thread %zu started", thread_id);

        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                // 等待任务或关闭信号
                condition_.wait(lock, [this]() {
                    return shutdown_ || !tasks_.empty();
                });

                // 关闭且队列为空，退出
                if (shutdown_ && tasks_.empty()) {
                    break;
                }

                // 取出任务
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
            }

            // 执行任务（锁外执行，避免阻塞其他线程）
            if (task) {
                try {
                    task();
                    tasks_processed_++;
                } catch (const std::exception& e) {
                    LOG_ERROR("CPU task exception in thread %zu: %s", thread_id, e.what());
                } catch (...) {
                    LOG_ERROR("CPU task unknown exception in thread %zu", thread_id);
                }
            }
        }

        LOG_DEBUG("CPU worker thread %zu stopped", thread_id);
    }

private:
    std::vector<std::thread> workers_;         // 工作线程
    std::queue<std::function<void()>> tasks_;  // 任务队列

    mutable std::mutex queue_mutex_;           // 队列互斥锁
    std::condition_variable condition_;        // 条件变量

    size_t max_queue_size_;                    // 最大队列长度
    bool shutdown_;                            // 关闭标志

    std::atomic<size_t> tasks_processed_;      // 已处理任务数
};

// ========== 构造函数实现 ==========

inline CpuThreadPool::CpuThreadPool(size_t num_threads, size_t max_queue_size)
    : max_queue_size_(max_queue_size), shutdown_(false), tasks_processed_(0) {

    // 自动检测CPU核心数
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // 默认4线程
        }
    }

    LOG_INFO("Creating CPU thread pool: %zu threads, max_queue=%zu",
             num_threads, max_queue_size);

    // 创建工作线程
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&CpuThreadPool::worker_thread, this, i);
    }
}

/**
 * @brief 全局CPU线程池实例（单例）
 *
 * 默认配置：
 * - 线程数 = CPU核心数
 * - 最大队列 = 10000
 */
inline CpuThreadPool& get_global_cpu_thread_pool() {
    static CpuThreadPool pool;  // 延迟初始化
    return pool;
}

#endif // CPU_THREAD_POOL_H
