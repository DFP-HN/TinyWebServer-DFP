#ifndef COROUTINE_TASK_H
#define COROUTINE_TASK_H

#include <coroutine>
#include <exception>
#include <utility>

/**
 * @brief 协程 Task 类型
 *
 * 用作协程的返回类型，支持挂起和恢复
 *
 * 使用示例：
 * @code
 * Task<int> async_compute() {
 *     co_return 42;
 * }
 *
 * Task<void> main_task() {
 *     int result = co_await async_compute();
 *     // result == 42
 * }
 * @endcode
 */
// 非 void 版本
template<typename T>
class Task {
public:
    struct promise_type {
        T value;
        std::exception_ptr exception;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T val) {
            value = std::move(val);
        }

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle(h) {}

    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }

    // 禁止拷贝
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 允许移动
    Task(Task&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief 恢复协程执行
     * @return true 如果协程未完成，false 如果已完成
     */
    bool resume() {
        if (!handle || handle.done()) {
            return false;
        }
        handle.resume();
        return !handle.done();
    }

    /**
     * @brief 检查协程是否已完成
     */
    bool is_done() const {
        return !handle || handle.done();
    }

    /**
     * @brief 获取协程结果
     * @throws 协程中抛出的异常
     */
    T get_result() {
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
        return std::move(handle.promise().value);
    }

    /**
     * @brief 使 Task 可以作为 Awaitable
     */
    bool await_ready() const noexcept {
        return is_done();
    }

    void await_suspend(std::coroutine_handle<> awaiter) {
        // 链接：当前协程完成时，恢复等待者
        // 这里需要更复杂的实现，暂时简化
        handle.resume();
        awaiter.resume();
    }

    T await_resume() {
        return get_result();
    }

private:
    handle_type handle;
};

// void 特化版本
template<>
class Task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle(h) {}

    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }

    // 禁止拷贝
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 允许移动
    Task(Task&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief 恢复协程执行
     * @return true 如果协程未完成，false 如果已完成
     */
    bool resume() {
        if (!handle || handle.done()) {
            return false;
        }
        handle.resume();
        return !handle.done();
    }

    /**
     * @brief 检查协程是否已完成
     */
    bool is_done() const {
        return !handle || handle.done();
    }

    /**
     * @brief 获取协程结果
     * @throws 协程中抛出的异常
     */
    void get_result() {
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
    }

    /**
     * @brief 使 Task 可以作为 Awaitable
     */
    bool await_ready() const noexcept {
        return is_done();
    }

    void await_suspend(std::coroutine_handle<> awaiter) {
        // 链接：当前协程完成时，恢复等待者
        // 这里需要更复杂的实现，暂时简化
        handle.resume();
        awaiter.resume();
    }

    void await_resume() {
        get_result();
    }

private:
    handle_type handle;
};

#endif // COROUTINE_TASK_H
