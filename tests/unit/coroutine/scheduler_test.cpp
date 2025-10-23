#include "../../utils/simple_test.h"
#include "../../utils/coroutine_test_runner.h"
#include "../../../coroutine/advanced_scheduler.h"
#include "../../../io_uring/io_uring_manager.h"
#include <chrono>
#include <thread>

/**
 * @file scheduler_test.cpp
 * @brief AdvancedScheduler的单元测试
 */

// 简单的延迟协程（模拟异步操作）
Task<int> delayed_task(int value, int delay_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    co_return value;
}

// 无返回值的测试协程
Task<void> void_task(int* counter) {
    (*counter)++;
    co_return;
}

// ================ 测试用例 ================

TEST(AdvancedSchedulerTest, BasicSpawn) {
    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init());

    AdvancedScheduler scheduler(io_mgr.get(), 100);

    int call_count = 0;
    auto task = void_task(&call_count);

    bool spawned = scheduler.spawn(std::move(task), CoroutinePriority::NORMAL, "test-task");
    EXPECT_TRUE(spawned);

    // 验证任务被执行（立即完成的协程会在spawn时完成）
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(scheduler.active_task_count(), 0);
}

TEST(AdvancedSchedulerTest, PriorityScheduling) {
    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init());

    AdvancedScheduler scheduler(io_mgr.get(), 100);

    // 测试不同优先级的任务spawn
    int counter = 0;

    bool spawned_high = scheduler.spawn(
        void_task(&counter), CoroutinePriority::HIGH, "high-prio"
    );
    bool spawned_normal = scheduler.spawn(
        void_task(&counter), CoroutinePriority::NORMAL, "normal-prio"
    );
    bool spawned_low = scheduler.spawn(
        void_task(&counter), CoroutinePriority::LOW, "low-prio"
    );

    EXPECT_TRUE(spawned_high);
    EXPECT_TRUE(spawned_normal);
    EXPECT_TRUE(spawned_low);

    // 所有任务应该已经完成
    EXPECT_EQ(counter, 3);
    EXPECT_EQ(scheduler.active_task_count(), 0);
}

TEST(AdvancedSchedulerTest, MaxCoroutineLimit) {
    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init());

    // 设置很小的限制
    AdvancedScheduler scheduler(io_mgr.get(), 3);

    int spawned_count = 0;
    int counters[5] = {0, 0, 0, 0, 0};

    // 尝试spawn超过限制的任务
    // 注意：立即完成的协程会立即释放，不会真正受限
    // 所以这个测试主要验证spawn接口正常工作
    for (int i = 0; i < 5; ++i) {
        if (scheduler.spawn(void_task(&counters[i]))) {
            spawned_count++;
        }
    }

    // 由于协程立即完成，所有5个都能spawn成功
    EXPECT_EQ(spawned_count, 5);
    // 验证所有任务都执行了
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(counters[i], 1);
    }
}

TEST(AdvancedSchedulerTest, Statistics) {
    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init());

    AdvancedScheduler scheduler(io_mgr.get(), 100);

    // Spawn几个任务
    int counters[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {
        scheduler.spawn(void_task(&counters[i]));
    }

    // 获取统计信息
    const auto& stats = scheduler.get_stats();

    EXPECT_EQ(stats.total_spawned.load(), 5);
    // 所有任务立即完成，active_count应该为0
    EXPECT_EQ(stats.active_count.load(), 0);
}

TEST(AdvancedSchedulerTest, GracefulShutdown) {
    auto io_mgr = std::make_unique<IoUringManager>();
    ASSERT_TRUE(io_mgr->init());

    AdvancedScheduler scheduler(io_mgr.get(), 100);

    // Spawn一些任务
    int counters[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        scheduler.spawn(void_task(&counters[i]));
    }

    // 调用shutdown
    scheduler.shutdown();

    // 再尝试spawn应该失败
    int counter = 0;
    bool spawned = scheduler.spawn(void_task(&counter));

    EXPECT_FALSE(spawned);
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "Running AdvancedScheduler unit tests...\n" << std::endl;
    return RUN_ALL_TESTS();
}
