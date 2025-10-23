#ifndef ADVANCED_SCHEDULER_H
#define ADVANCED_SCHEDULER_H

#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>
#include <liburing.h>
#include "task.h"
#include "io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../log/log.h"

/**
 * @brief 协程优先级
 */
enum class CoroutinePriority {
    HIGH = 0,      // 高优先级（如：短连接、控制请求）
    NORMAL = 1,    // 普通优先级（默认）
    LOW = 2        // 低优先级（如：后台任务、清理）
};

/**
 * @brief 协程统计信息
 */
struct CoroStats {
    std::atomic<size_t> total_spawned{0};      // 总创建数
    std::atomic<size_t> total_completed{0};    // 总完成数
    std::atomic<size_t> active_count{0};       // 当前活跃数
    std::atomic<size_t> high_priority_count{0};
    std::atomic<size_t> normal_priority_count{0};
    std::atomic<size_t> low_priority_count{0};
    std::atomic<size_t> io_wait_count{0};      // 等待I/O的协程数
    std::atomic<uint64_t> total_execution_time_us{0};  // 总执行时间（微秒）

    void print_stats() const {
        LOG_INFO("=== Coroutine Stats ===");
        LOG_INFO("Total spawned: %zu", total_spawned.load());
        LOG_INFO("Total completed: %zu", total_completed.load());
        LOG_INFO("Active count: %zu", active_count.load());
        LOG_INFO("High priority: %zu", high_priority_count.load());
        LOG_INFO("Normal priority: %zu", normal_priority_count.load());
        LOG_INFO("Low priority: %zu", low_priority_count.load());
        LOG_INFO("I/O waiting: %zu", io_wait_count.load());
        LOG_INFO("Avg execution time: %.2f ms",
                 total_completed.load() > 0 ?
                 (double)total_execution_time_us.load() / total_completed.load() / 1000.0 : 0.0);
    }
};

/**
 * @brief 协程元数据（用于调度和管理）
 */
struct CoroMetadata {
    size_t id;                          // 协程唯一ID
    CoroutinePriority priority;         // 优先级
    std::chrono::steady_clock::time_point spawn_time;  // 创建时间
    std::chrono::steady_clock::time_point resume_time; // 最后恢复时间
    bool is_waiting_io;                 // 是否在等待I/O
    std::string debug_name;             // 调试名称（可选）

    CoroMetadata()
        : id(0), priority(CoroutinePriority::NORMAL),
          spawn_time(std::chrono::steady_clock::now()),
          resume_time(spawn_time),
          is_waiting_io(false) {}
};

/**
 * @brief 协程任务包装器（包含任务和元数据）
 */
struct CoroTaskWrapper {
    Task<void> task;
    CoroMetadata metadata;

    CoroTaskWrapper(Task<void> t, CoroutinePriority prio = CoroutinePriority::NORMAL,
                    const char* name = nullptr)
        : task(std::move(t)) {
        metadata.priority = prio;
        if (name) {
            metadata.debug_name = name;
        }
    }

    // 用于优先级队列的比较（优先级高的排前面）
    bool operator<(const CoroTaskWrapper& other) const {
        return metadata.priority > other.metadata.priority;  // 注意：反向比较
    }
};

/**
 * @brief 增强型协程调度器
 *
 * 新增特性：
 * - 协程数量限制
 * - 优先级调度
 * - 统计信息收集
 * - 协程生命周期管理
 * - 优雅关闭
 *
 * 使用示例：
 * @code
 * AdvancedScheduler scheduler(io_mgr, 10000);  // 最多10000个协程
 * scheduler.spawn(handle_connection(fd), CoroutinePriority::NORMAL, "conn-handler");
 * scheduler.run();
 * @endcode
 */
class AdvancedScheduler {
public:
    /**
     * @brief 构造函数
     * @param mgr io_uring 管理器
     * @param max_coros 最大协程数（0=无限制）
     */
    explicit AdvancedScheduler(IoUringManager* mgr, size_t max_coros = 10000)
        : io_uring_mgr(mgr), max_coroutines(max_coros), next_coro_id(1),
          shutdown_requested(false) {
        LOG_INFO("AdvancedScheduler created (max_coros=%zu)", max_coros);
    }

    /**
     * @brief 启动一个新协程（带优先级和名称）
     * @param task 协程任务
     * @param priority 优先级
     * @param debug_name 调试名称（可选）
     * @return true=成功, false=超过最大协程数限制
     */
    bool spawn(Task<void> task, CoroutinePriority priority = CoroutinePriority::NORMAL,
               const char* debug_name = nullptr) {
        if (shutdown_requested.load()) {
            LOG_WARN("Cannot spawn coroutine: scheduler is shutting down");
            return false;
        }

        // 检查协程数量限制
        if (max_coroutines > 0 && stats.active_count.load() >= max_coroutines) {
            LOG_ERROR("Cannot spawn coroutine: max limit reached (%zu)", max_coroutines);
            return false;
        }

        // 创建包装器
        CoroTaskWrapper wrapper(std::move(task), priority, debug_name);
        wrapper.metadata.id = next_coro_id++;

        // 更新统计
        stats.total_spawned++;
        stats.active_count++;
        update_priority_count(priority, 1);

        if (debug_name) {
            LOG_DEBUG("Spawned coroutine #%zu '%s' (priority=%d)",
                      wrapper.metadata.id, debug_name, (int)priority);
        }

        // 尝试立即执行一次
        if (!wrapper.task.resume()) {
            // 协程立即完成
            on_coroutine_completed(wrapper.metadata);
            return true;
        }

        // 添加到任务列表
        tasks.push_back(std::move(wrapper));
        return true;
    }

    /**
     * @brief 运行事件循环
     *
     * 持续运行直到所有协程完成或shutdown()被调用
     */
    void run() {
        LOG_INFO("AdvancedScheduler started (active_count=%zu)", stats.active_count.load());

        while (!tasks.empty() && !shutdown_requested.load()) {
            // 1. 提交所有待处理的 I/O 请求
            int submitted = io_uring_mgr->submit_all();
            if (submitted < 0) {
                LOG_ERROR("io_uring_submit failed");
                break;
            }

            // 2. 等待至少一个完成事件（带超时）
            int ready = io_uring_mgr->wait_completions(1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;  // 被信号中断，继续
                }
                LOG_ERROR("io_uring_wait_completions failed");
                break;
            }

            // 3. 处理所有完成事件
            int processed = io_uring_mgr->process_completions(
                [this](struct io_uring_cqe *cqe) {
                    handle_completion(cqe);
                }
            );

            if (processed < 0) {
                LOG_ERROR("process_completions failed");
                break;
            }

            // 4. 清理已完成的协程
            cleanup_finished_tasks();

            // 5. 定期打印统计信息（可选）
            static size_t iteration_count = 0;
            if (++iteration_count % 10000 == 0) {
                LOG_DEBUG("Scheduler iteration %zu, active_count=%zu",
                          iteration_count, stats.active_count.load());
            }
        }

        LOG_INFO("AdvancedScheduler stopped");
        if (!tasks.empty()) {
            LOG_WARN("Scheduler stopped with %zu active coroutines", tasks.size());
        }
    }

    /**
     * @brief 请求关闭调度器
     *
     * 设置标志位，run()将在当前迭代后退出
     */
    void shutdown() {
        LOG_INFO("Shutdown requested");
        shutdown_requested.store(true);
    }

    /**
     * @brief 获取统计信息
     */
    const CoroStats& get_stats() const {
        return stats;
    }

    /**
     * @brief 打印统计信息
     */
    void print_stats() const {
        stats.print_stats();
    }

    /**
     * @brief 获取活跃协程数量
     */
    size_t active_task_count() const {
        return stats.active_count.load();
    }

    /**
     * @brief 检查是否有待处理的 I/O
     */
    bool has_pending_io() const {
        return io_uring_mgr->get_sq_pending() > 0 ||
               io_uring_mgr->get_cq_ready() > 0;
    }

private:
    /**
     * @brief 处理 io_uring 完成事件
     */
    void handle_completion(struct io_uring_cqe *cqe) {
        // user_data 存储的是 Awaiter 的指针
        uint64_t user_data = (uint64_t)io_uring_cqe_get_data(cqe);

        if (user_data == 0) {
            LOG_WARN("Received CQE with NULL user_data");
            return;
        }

        // 调用 Awaiter 的 complete() 方法
        auto* awaiter = reinterpret_cast<BaseAwaiter*>(user_data);
        awaiter->complete(cqe->res);

        // 更新I/O等待计数
        stats.io_wait_count--;
    }

    /**
     * @brief 清理已完成的任务
     */
    void cleanup_finished_tasks() {
        auto it = tasks.begin();
        while (it != tasks.end()) {
            if (it->task.is_done()) {
                on_coroutine_completed(it->metadata);
                it = tasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief 协程完成时的回调
     */
    void on_coroutine_completed(const CoroMetadata& meta) {
        // 计算执行时间
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - meta.spawn_time);

        // 更新统计
        stats.total_completed++;
        stats.active_count--;
        update_priority_count(meta.priority, -1);
        stats.total_execution_time_us += duration.count();

        if (!meta.debug_name.empty()) {
            LOG_DEBUG("Coroutine #%zu '%s' completed in %lld us",
                      meta.id, meta.debug_name.c_str(), duration.count());
        }
    }

    /**
     * @brief 更新优先级计数
     */
    void update_priority_count(CoroutinePriority prio, int delta) {
        switch (prio) {
            case CoroutinePriority::HIGH:
                stats.high_priority_count += delta;
                break;
            case CoroutinePriority::NORMAL:
                stats.normal_priority_count += delta;
                break;
            case CoroutinePriority::LOW:
                stats.low_priority_count += delta;
                break;
        }
    }

private:
    IoUringManager* io_uring_mgr;
    std::vector<CoroTaskWrapper> tasks;  // 改用vector（后续可优化为优先级队列）
    size_t max_coroutines;
    std::atomic<size_t> next_coro_id;
    std::atomic<bool> shutdown_requested;
    CoroStats stats;
};

#endif // ADVANCED_SCHEDULER_H
