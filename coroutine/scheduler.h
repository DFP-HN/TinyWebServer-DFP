#ifndef COROUTINE_SCHEDULER_H
#define COROUTINE_SCHEDULER_H

#include <vector>
#include <unordered_map>
#include <memory>
#include <liburing.h>
#include "task.h"
#include "io_awaiter.h"
#include "../io_uring/io_uring_manager.h"
#include "../log/log.h"

/**
 * @brief 协程调度器
 *
 * 管理所有协程的生命周期，调度 io_uring 事件
 *
 * 使用示例：
 * @code
 * CoroScheduler scheduler(io_uring_mgr);
 *
 * // 启动协程
 * scheduler.spawn(handle_connection(sockfd));
 *
 * // 运行事件循环
 * scheduler.run();
 * @endcode
 */
class CoroScheduler {
public:
    /**
     * @brief 构造函数
     * @param mgr io_uring 管理器
     */
    explicit CoroScheduler(IoUringManager* mgr)
        : io_uring_mgr(mgr) {}

    /**
     * @brief 启动一个新协程
     * @param task 要启动的协程任务
     */
    void spawn(Task<void> task) {
        fprintf(stderr, "[DEBUG] CoroScheduler::spawn: START\n");
        fflush(stderr);
        tasks.push_back(std::move(task));
        fprintf(stderr, "[DEBUG] CoroScheduler::spawn: Task added, size=%zu\n", tasks.size());
        fflush(stderr);

        // 尝试立即执行一次
        fprintf(stderr, "[DEBUG] CoroScheduler::spawn: Resuming task...\n");
        fflush(stderr);
        auto& new_task = tasks.back();
        if (!new_task.resume()) {
            // 协程立即完成，移除
            fprintf(stderr, "[DEBUG] CoroScheduler::spawn: Task completed immediately\n");
            fflush(stderr);
            tasks.pop_back();
        }
        fprintf(stderr, "[DEBUG] CoroScheduler::spawn: DONE\n");
        fflush(stderr);
    }

    /**
     * @brief 运行事件循环
     *
     * 持续运行直到所有协程完成且没有待处理的 I/O
     */
    void run() {
        fprintf(stderr, "[DEBUG] CoroScheduler::run: START\n");
        fflush(stderr);
        LOG_INFO("Coroutine scheduler starting...");

        // 只要有协程在运行，就继续循环
        // 注意：不依赖 has_pending_io()，因为它只检查 SQ 队列，
        //      无法准确反映飞行中的 I/O 请求数量
        while (!tasks.empty()) {
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Loop iteration, tasks=%zu\n",
                    tasks.size());
            fflush(stderr);

            // 1. 提交所有待处理的 I/O 请求
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Submitting I/O...\n");
            fflush(stderr);
            int submitted = io_uring_mgr->submit_all();
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Submitted %d requests\n", submitted);
            fflush(stderr);
            if (submitted < 0) {
                LOG_ERROR("io_uring_submit failed");
                break;
            }

            // 2. 等待至少一个完成事件
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Waiting for completions...\n");
            fflush(stderr);
            int ready = io_uring_mgr->wait_completions(1);
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Got %d completions\n", ready);
            fflush(stderr);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;  // 被信号中断，继续
                }
                LOG_ERROR("io_uring_wait_completions failed");
                break;
            }

            // 3. 处理所有完成事件
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Processing completions...\n");
            fflush(stderr);
            int processed = io_uring_mgr->process_completions(
                [this](struct io_uring_cqe *cqe) {
                    fprintf(stderr, "[DEBUG] CoroScheduler: Processing CQE, user_data=%p, res=%d\n",
                            (void*)io_uring_cqe_get_data(cqe), cqe->res);
                    fflush(stderr);
                    handle_completion(cqe);
                }
            );
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Processed %d completions\n", processed);
            fflush(stderr);

            if (processed < 0) {
                LOG_ERROR("process_completions failed");
                break;
            }

            // 4. 清理已完成的协程
            // 注意：不需要手动恢复协程，I/O 完成时 Awaiter 会自动恢复
            fprintf(stderr, "[DEBUG] CoroScheduler::run: Cleaning up finished tasks...\n");
            fflush(stderr);
            cleanup_finished_tasks();
        }

        fprintf(stderr, "[DEBUG] CoroScheduler::run: DONE\n");
        fflush(stderr);
        LOG_INFO("Coroutine scheduler stopped");
    }

    /**
     * @brief 获取活跃协程数量
     */
    size_t active_task_count() const {
        return tasks.size();
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

        // 尝试不同类型的 Awaiter
        // 注意：这里需要一种更好的方法来识别 Awaiter 类型
        // 可以在 user_data 中编码类型信息，或使用虚函数

        // 简化版本：假设所有 Awaiter 都有 complete() 方法
        // 实际实现中，需要更安全的类型识别机制

        // 这里使用一个基类指针的方式
        // 假设所有 Awaiter 都继承自 IAwaiter
        auto* awaiter = reinterpret_cast<BaseAwaiter*>(user_data);
        awaiter->complete(cqe->res);
    }

    /**
     * @brief 恢复所有就绪的任务
     */
    void resume_ready_tasks() {
        for (auto& task : tasks) {
            if (!task.is_done()) {
                task.resume();
            }
        }
    }

    /**
     * @brief 清理已完成的任务
     */
    void cleanup_finished_tasks() {
        tasks.erase(
            std::remove_if(tasks.begin(), tasks.end(),
                [](Task<void>& t) { return t.is_done(); }),
            tasks.end()
        );
    }

private:
    IoUringManager* io_uring_mgr;
    std::vector<Task<void>> tasks;
};

#endif // COROUTINE_SCHEDULER_H
