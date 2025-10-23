#include "../utils/simple_test.h"
#include "../utils/coroutine_test_runner.h"
#include "../../threadpool/cpu_thread_pool.h"
#include "../../coroutine/advanced_scheduler.h"
#include <chrono>
#include <atomic>
#include <vector>

/**
 * @file performance_benchmark_test.cpp
 * @brief 性能基准测试
 *
 * 测试各个组件的性能指标
 */

// 辅助函数：格式化数字
std::string format_number(long long num) {
    std::string str = std::to_string(num);
    int insert_pos = str.length() - 3;
    while (insert_pos > 0) {
        str.insert(insert_pos, ",");
        insert_pos -= 3;
    }
    return str;
}

TEST(PerformanceTest, CoroutineCreationOverhead) {
    std::cout << "\n=== Coroutine Creation Overhead ===" << std::endl;

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto task = []() -> Task<int> {
            co_return 42;
        }();

        // 启动并销毁
        auto handle = task.get_handle();
        if (handle) {
            handle.resume();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    long long ops_per_sec = static_cast<long long>((iterations * 1000000.0) / duration.count());

    std::cout << "Iterations: " << format_number(iterations) << std::endl;
    std::cout << "Total time: " << duration.count() / 1000.0 << " ms" << std::endl;
    std::cout << "Avg time per coroutine: " << avg_time_us << " μs" << std::endl;
    std::cout << "Throughput: " << format_number(ops_per_sec) << " coroutines/sec" << std::endl;

    // 基准：每个协程创建应该在几微秒内
    EXPECT_LT(avg_time_us, 100.0);
}

TEST(PerformanceTest, ThreadPoolThroughput) {
    std::cout << "\n=== Thread Pool Throughput ===" << std::endl;

    const int task_count = 10000;
    CpuThreadPool pool(8, task_count);

    std::atomic<int> completed{0};
    auto start = std::chrono::high_resolution_clock::now();

    // 提交大量轻量级任务
    for (int i = 0; i < task_count; ++i) {
        pool.enqueue([&completed]() {
            completed.fetch_add(1);
        });
    }

    // 等待所有任务完成
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    long long ops_per_sec = static_cast<long long>((task_count * 1000.0) / duration.count());

    std::cout << "Tasks: " << format_number(task_count) << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "Throughput: " << format_number(ops_per_sec) << " tasks/sec" << std::endl;

    EXPECT_EQ(completed.load(), task_count);
}

TEST(PerformanceTest, SchedulerOverhead) {
    std::cout << "\n=== Scheduler Overhead ===" << std::endl;

    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init(256));

    AdvancedScheduler scheduler(io_mgr.get(), 10000);

    const int coro_count = 1000;
    std::atomic<int> completed{0};

    auto start = std::chrono::high_resolution_clock::now();

    // Spawn大量协程
    for (int i = 0; i < coro_count; ++i) {
        auto task = [&completed]() -> Task<void> {
            completed.fetch_add(1);
            co_return;
        }();

        scheduler.spawn(std::move(task), CoroutinePriority::NORMAL);
    }

    // 运行调度器直到所有协程完成
    while (completed.load() < coro_count) {
        scheduler.run_once(10);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    const auto& stats = scheduler.get_stats();

    std::cout << "Coroutines: " << format_number(coro_count) << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "Avg time per coroutine: "
              << (duration.count() * 1000.0 / coro_count) << " μs" << std::endl;
    std::cout << "Peak active: " << stats.peak_active_count << std::endl;
    std::cout << "Total completed: " << stats.total_completed << std::endl;

    EXPECT_EQ(completed.load(), coro_count);
}

TEST(PerformanceTest, MemoryAllocation) {
    std::cout << "\n=== Memory Allocation Benchmark ===" << std::endl;

    const int iterations = 100000;
    std::vector<Task<int>> tasks;
    tasks.reserve(iterations);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        tasks.push_back([]() -> Task<int> {
            co_return 42;
        }());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Task objects created: " << format_number(iterations) << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "Allocation rate: "
              << format_number((iterations * 1000) / duration.count()) << " objs/sec" << std::endl;

    // 大致估算内存使用
    size_t approx_memory_mb = (tasks.capacity() * sizeof(Task<int>)) / (1024 * 1024);
    std::cout << "Approx memory: " << approx_memory_mb << " MB" << std::endl;

    EXPECT_GT(tasks.size(), 0);
}

TEST(PerformanceTest, ConcurrentQueueContention) {
    std::cout << "\n=== Concurrent Queue Contention ===" << std::endl;

    const int task_count = 5000;
    const int thread_count = 8;

    CpuThreadPool pool(thread_count, task_count);

    std::atomic<int> completed{0};
    std::vector<std::thread> producers;

    auto start = std::chrono::high_resolution_clock::now();

    // 多个生产者线程并发提交任务
    for (int t = 0; t < thread_count; ++t) {
        producers.emplace_back([&pool, &completed, task_count, thread_count]() {
            int tasks_per_thread = task_count / thread_count;

            for (int i = 0; i < tasks_per_thread; ++i) {
                pool.enqueue([&completed]() {
                    completed.fetch_add(1);
                });
            }
        });
    }

    // 等待所有生产者完成
    for (auto& t : producers) {
        t.join();
    }

    // 等待所有任务完成
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Tasks: " << format_number(task_count) << std::endl;
    std::cout << "Producer threads: " << thread_count << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "Throughput: "
              << format_number((task_count * 1000) / duration.count()) << " tasks/sec" << std::endl;

    EXPECT_EQ(completed.load(), task_count);
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "========================================" << std::endl;
    std::cout << "Performance Benchmark Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    return RUN_ALL_TESTS();
}
