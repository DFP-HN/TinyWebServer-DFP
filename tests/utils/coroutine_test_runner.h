#ifndef TESTS_UTILS_COROUTINE_TEST_RUNNER_H
#define TESTS_UTILS_COROUTINE_TEST_RUNNER_H

#include "../../coroutine/task.h"
#include "../../io_uring/io_uring_manager.h"
#include <chrono>
#include <stdexcept>
#include <memory>

/**
 * @brief 协程测试运行器
 *
 * 用于在测试环境中同步运行协程，处理事件循环和超时
 *
 * 使用示例：
 * @code
 * TEST(MyTest, CoroTest) {
 *     CoroTestRunner runner;
 *     Task<int> task = my_coroutine();
 *     int result = runner.run_sync(std::move(task));
 *     EXPECT_EQ(result, 42);
 * }
 * @endcode
 */
class CoroTestRunner {
public:
    CoroTestRunner()
        : io_uring_manager_(std::make_unique<IoUringManager>())
        , default_timeout_ms_(5000)
    {
        if (!io_uring_manager_->init()) {
            throw std::runtime_error("Failed to initialize IoUringManager for testing");
        }
    }

    ~CoroTestRunner() {
        // 清理资源
    }

    /**
     * @brief 同步运行协程（非void返回值）
     *
     * @tparam T 协程返回值类型
     * @param task 要运行的协程
     * @param timeout_ms 超时时间（毫秒），默认5秒
     * @return T 协程的返回值
     * @throws std::runtime_error 如果超时或协程抛出异常
     */
    template<typename T>
    T run_sync(Task<T> task, int timeout_ms = -1) {
        if (timeout_ms < 0) {
            timeout_ms = default_timeout_ms_;
        }

        // 启动协程
        auto handle = task.get_handle();
        if (!handle || handle.done()) {
            throw std::runtime_error("Invalid coroutine handle");
        }

        handle.resume();

        // 运行事件循环直到协程完成或超时
        auto start_time = std::chrono::steady_clock::now();
        while (!handle.done()) {
            // 检查超时
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                throw std::runtime_error("Coroutine execution timeout");
            }

            // 泵送io_uring事件（超时1ms，避免阻塞）
            pump_io_uring(1);
        }

        // 获取结果或重新抛出异常
        return task.get_result();
    }

    /**
     * @brief 同步运行void协程
     */
    void run_sync_void(Task<void> task, int timeout_ms = -1) {
        if (timeout_ms < 0) {
            timeout_ms = default_timeout_ms_;
        }

        auto handle = task.get_handle();
        if (!handle || handle.done()) {
            throw std::runtime_error("Invalid coroutine handle");
        }

        handle.resume();

        auto start_time = std::chrono::steady_clock::now();
        while (!handle.done()) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                throw std::runtime_error("Coroutine execution timeout");
            }

            pump_io_uring(1);
        }

        // 检查是否有异常
        task.get_result_void();
    }

    /**
     * @brief 手动泵送io_uring事件
     *
     * @param timeout_ms 等待超时（毫秒），0表示非阻塞（注意：当前实现忽略超时参数）
     */
    void pump_io_uring(int timeout_ms = 0) {
        (void)timeout_ms;  // 忽略超时参数
        if (!io_uring_manager_) {
            return;
        }

        // 处理所有待处理的完成事件
        io_uring_manager_->process_completions([](struct io_uring_cqe* cqe) {
            // 简单地标记已看到（实际处理由awaiter完成）
            (void)cqe;
        });
    }

    /**
     * @brief 获取IoUringManager实例（用于创建awaiter）
     */
    IoUringManager* get_io_uring_manager() {
        return io_uring_manager_.get();
    }

    /**
     * @brief 设置默认超时时间
     */
    void set_default_timeout(int timeout_ms) {
        default_timeout_ms_ = timeout_ms;
    }

    /**
     * @brief 重置测试环境（清理未完成的操作）
     */
    void reset() {
        // 简单实现：处理所有待处理的完成事件
        pump_io_uring(0);
    }

private:
    std::unique_ptr<IoUringManager> io_uring_manager_;
    int default_timeout_ms_;
};

#endif // TESTS_UTILS_COROUTINE_TEST_RUNNER_H
