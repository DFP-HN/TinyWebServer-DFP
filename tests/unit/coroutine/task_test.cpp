#include "../../utils/simple_test.h"
#include "../../utils/coroutine_test_runner.h"
#include "../../../coroutine/task.h"
#include <string>

/**
 * @file task_test.cpp
 * @brief 协程Task类型的单元测试
 */

// 简单的协程函数
Task<int> simple_coroutine() {
    co_return 42;
}

// 带参数的协程
Task<int> add_async(int a, int b) {
    co_return a + b;
}

// 字符串返回的协程
Task<std::string> hello_async(const std::string& name) {
    co_return "Hello, " + name;
}

// void协程
Task<void> void_coroutine() {
    // Do nothing
    co_return;
}

// 抛出异常的协程
Task<int> exception_coroutine() {
    throw std::runtime_error("Test exception");
    co_return 0;
}

// 链式调用协程
Task<int> chain_coroutine() {
    int result = co_await simple_coroutine();
    result += co_await add_async(10, 20);
    co_return result;
}

// ================ 测试用例 ================

TEST(TaskTest, BasicReturn) {
    CoroTestRunner runner;

    Task<int> task = simple_coroutine();
    int result = runner.run_sync(std::move(task));

    EXPECT_EQ(result, 42);
}

TEST(TaskTest, WithParameters) {
    CoroTestRunner runner;

    Task<int> task = add_async(100, 200);
    int result = runner.run_sync(std::move(task));

    EXPECT_EQ(result, 300);
}

TEST(TaskTest, StringReturn) {
    CoroTestRunner runner;

    Task<std::string> task = hello_async("World");
    std::string result = runner.run_sync(std::move(task));

    EXPECT_STREQ(result.c_str(), "Hello, World");
}

TEST(TaskTest, VoidReturn) {
    CoroTestRunner runner;

    Task<void> task = void_coroutine();

    // 应该正常运行不抛异常
    try {
        runner.run_sync_void(std::move(task));
        EXPECT_TRUE(true);  // 成功
    } catch (...) {
        EXPECT_TRUE(false);  // 不应该抛异常
    }
}

TEST(TaskTest, ExceptionPropagation) {
    CoroTestRunner runner;

    Task<int> task = exception_coroutine();

    // 应该捕获协程内部的异常
    bool caught_exception = false;
    try {
        runner.run_sync(std::move(task));
    } catch (const std::runtime_error& e) {
        caught_exception = true;
        EXPECT_STREQ(e.what(), "Test exception");
    }

    EXPECT_TRUE(caught_exception);
}

TEST(TaskTest, ChainedAwait) {
    CoroTestRunner runner;

    Task<int> task = chain_coroutine();
    int result = runner.run_sync(std::move(task));

    // 42 + (10 + 20) = 72
    EXPECT_EQ(result, 72);
}

TEST(TaskTest, MultipleTasksSequential) {
    CoroTestRunner runner;

    // 顺序运行多个任务
    int sum = 0;

    sum += runner.run_sync(simple_coroutine());
    sum += runner.run_sync(add_async(5, 10));
    sum += runner.run_sync(add_async(3, 7));

    // 42 + 15 + 10 = 67
    EXPECT_EQ(sum, 67);
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "Running Task unit tests...\n" << std::endl;
    return RUN_ALL_TESTS();
}
