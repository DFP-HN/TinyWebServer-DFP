#include "../../utils/simple_test.h"
#include "../../../threadpool/cpu_thread_pool.h"
#include <atomic>
#include <chrono>
#include <thread>

/**
 * @file cpu_thread_pool_test.cpp
 * @brief CpuThreadPool的单元测试
 */

TEST(CpuThreadPoolTest, BasicEnqueue) {
    CpuThreadPool pool(4, 10);

    std::atomic<int> counter{0};

    // 提交一个简单的任务
    bool enqueued = pool.enqueue([&counter]() {
        counter++;
    });

    EXPECT_TRUE(enqueued);

    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(counter.load(), 1);
}

TEST(CpuThreadPoolTest, MultipleTasksConcurrent) {
    CpuThreadPool pool(4, 100);

    std::atomic<int> counter{0};
    const int task_count = 50;

    // 提交多个任务
    for (int i = 0; i < task_count; ++i) {
        pool.enqueue([&counter]() {
            counter.fetch_add(1);
        });
    }

    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(counter.load(), task_count);
}

TEST(CpuThreadPoolTest, QueueLimit) {
    // 创建小队列的线程池
    CpuThreadPool pool(2, 5);

    std::atomic<int> enqueued_count{0};

    // 尝试提交超过队列限制的任务
    for (int i = 0; i < 20; ++i) {
        if (pool.enqueue([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        })) {
            enqueued_count++;
        }
    }

    // 不应该能提交所有20个任务
    EXPECT_LT(enqueued_count.load(), 20);
}

TEST(CpuThreadPoolTest, TaskExecution) {
    CpuThreadPool pool(4, 10);

    std::atomic<bool> task_executed{false};
    std::atomic<int> result{0};

    pool.enqueue([&task_executed, &result]() {
        result.store(42);
        task_executed.store(true);
    });

    // 等待任务执行
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(task_executed.load());
    EXPECT_EQ(result.load(), 42);
}

TEST(CpuThreadPoolTest, Statistics) {
    CpuThreadPool pool(4, 10);

    // 提交几个任务
    for (int i = 0; i < 5; ++i) {
        pool.enqueue([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }

    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 打印统计信息（目视检查）
    pool.print_stats();

    // 队列大小应该为0（所有任务已完成）
    EXPECT_EQ(pool.queue_size(), 0);
}

TEST(CpuThreadPoolTest, ThreadCount) {
    size_t thread_count = 8;
    CpuThreadPool pool(thread_count, 10);

    std::atomic<int> concurrent_count{0};
    std::atomic<int> max_concurrent{0};

    // 提交大量任务，计算最大并发数
    for (int i = 0; i < 20; ++i) {
        pool.enqueue([&concurrent_count, &max_concurrent]() {
            int current = concurrent_count.fetch_add(1) + 1;

            // 更新最大并发数
            int expected = max_concurrent.load();
            while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
                expected = max_concurrent.load();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            concurrent_count.fetch_sub(1);
        });
    }

    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 最大并发数应该接近线程数（但不超过）
    EXPECT_LE(max_concurrent.load(), thread_count);
    EXPECT_GT(max_concurrent.load(), 0);
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "Running CpuThreadPool unit tests...\n" << std::endl;
    return RUN_ALL_TESTS();
}
