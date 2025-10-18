#ifndef WORK_STEALING_POOL_H
#define WORK_STEALING_POOL_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <thread>
#include <optional>
#include <cstdint>
#include <memory>
#include "chase_lev_deque.h"
#include "../CGImysql/sql_connection_pool.h"

// 前向声明
template <typename T> class WorkStealingPool;
template <typename T> class WorkerThread;

/**
 * IO_Task - 任务结构体
 * 封装 HTTP 连接处理任务
 */
template <typename T>
struct IO_Task
{
    T* request;              // HTTP 连接对象指针
    int state;               // 0=读, 1=写 (仅 Reactor 模式使用)
    int actor_model;         // 0=Proactor, 1=Reactor
    connection_pool* connPool;  // 数据库连接池

    // 默认构造函数
    IO_Task() : request(nullptr), state(0), actor_model(0), connPool(nullptr) {}

    // 构造函数
    IO_Task(T* req, int st, int model, connection_pool* pool)
        : request(req), state(st), actor_model(model), connPool(pool)
    {
    }

    // 执行任务的逻辑
    void execute();
};

// 全局状态变量
namespace work_stealing_globals
{
    // 工作通告板：每个比特位对应一个工作线程
    extern std::atomic<uint64_t> work_announcement_board;

    // 全局休眠锁和条件变量
    extern std::mutex g_sleep_mutex;
    extern std::condition_variable g_sleeper_cv;

    // 阈值常量
    constexpr int ANNOUNCE_THRESHOLD = 4;    // 任务数超过此值时通告
    constexpr int REVOKE_THRESHOLD = 2;      // 任务数低于此值时撤销通告
    constexpr int MAX_WORKERS = 64;          // 最大工作线程数（受 uint64_t 限制）
}

/**
 * WorkerThread - 工作线程类
 * 每个线程维护自己的任务队列，支持工作窃取
 */
template <typename T>
class WorkerThread
{
private:
    int thread_id;                              // 线程 ID (0 到 n-1)
    ChaseLevDeque<IO_Task<T>*> local_queue;    // 本地任务队列
    std::vector<WorkerThread<T>*>* all_workers; // 所有工作线程的指针数组
    std::atomic<bool>* shutdown_flag;           // 关闭标志

    // 尝试从其他线程窃取任务
    IO_Task<T>* try_steal()
    {
        using namespace work_stealing_globals;

        uint64_t board = work_announcement_board.load(std::memory_order_acquire);

        if (board == 0)
        {
            // 没有可窃取的工作
            return nullptr;
        }

        // 遍历所有置位的线程
        for (int victim_id = 0; victim_id < (int)all_workers->size(); ++victim_id)
        {
            if (victim_id == thread_id)
                continue;  // 不窃取自己

            // 检查该线程是否通告了工作
            if ((board & (1ULL << victim_id)) == 0)
                continue;

            // 尝试从受害者窃取
            auto stolen = (*all_workers)[victim_id]->local_queue.steal_top();
            if (stolen.has_value())
            {
                return stolen.value();
            }
        }

        return nullptr;
    }

    // 执行任务
    void execute_task(IO_Task<T>* task)
    {
        if (!task || !task->request)
            return;

        task->execute();
        delete task;  // 释放任务对象
    }

    // 更新工作通告板
    void update_announcement(int64_t queue_size)
    {
        using namespace work_stealing_globals;

        if (queue_size > ANNOUNCE_THRESHOLD)
        {
            // 通告有工作
            uint64_t old_board = work_announcement_board.fetch_or(
                1ULL << thread_id, std::memory_order_release);

            // 如果通告板从 0 变为非 0，唤醒所有休眠线程
            if (old_board == 0)
            {
                g_sleeper_cv.notify_all();
            }
        }
        else if (queue_size < REVOKE_THRESHOLD)
        {
            // 撤销通告
            work_announcement_board.fetch_and(
                ~(1ULL << thread_id), std::memory_order_release);
        }
    }

public:
    WorkerThread(int id, std::vector<WorkerThread<T>*>* workers, std::atomic<bool>* shutdown)
        : thread_id(id), all_workers(workers), shutdown_flag(shutdown)
    {
    }

    // 主运行循环
    void run()
    {
        using namespace work_stealing_globals;

        while (!shutdown_flag->load(std::memory_order_relaxed))
        {
            IO_Task<T>* task = nullptr;
            bool found_work = false;

            // 第一步：执行本地任务
            while (true)
            {
                auto local_task = local_queue.pop_bottom();
                if (!local_task.has_value())
                    break;

                task = local_task.value();
                execute_task(task);
                found_work = true;

                // 更新通告状态
                int64_t size = local_queue.size();
                update_announcement(size);
            }

            // 如果处理了本地任务，继续下一轮（可能有更多任务）
            if (found_work)
                continue;

            // 第二步：尝试窃取任务
            task = try_steal();
            if (task != nullptr)
            {
                execute_task(task);
                continue;  // 窃取成功，重新开始循环检查本地队列
            }

            // 第三步：没有本地任务，也没窃取到任务，需要休眠
            // 在持有锁的情况下再次检查，避免lost wakeup
            std::unique_lock<std::mutex> lock(g_sleep_mutex);

            // 再次尝试窃取（在锁保护下），避免与push_task竞争
            task = try_steal();
            if (task != nullptr)
            {
                lock.unlock();
                execute_task(task);
                continue;
            }

            // 确认没有工作后，进入休眠
            if (!shutdown_flag->load(std::memory_order_relaxed))
            {
                // 等待被唤醒（有新任务或关闭信号）
                g_sleeper_cv.wait(lock, [this]() {
                    return shutdown_flag->load(std::memory_order_relaxed) ||
                           !all_workers->at(thread_id)->local_queue.empty();
                });
            }
        }
    }

    // 向本地队列添加任务
    void push_task(IO_Task<T>* task)
    {
        using namespace work_stealing_globals;

        // 先添加任务到队列
        local_queue.push_bottom(task);

        // 更新通告状态
        int64_t size = local_queue.size();
        update_announcement(size);

        // 获取锁并唤醒线程（与run()中的休眠使用同一个锁）
        {
            std::lock_guard<std::mutex> lock(g_sleep_mutex);
            // 在锁保护下发送通知，确保不会lost wakeup
            g_sleeper_cv.notify_all();
        }
    }

    friend class WorkStealingPool<T>;
};

/**
 * WorkStealingPool - 工作窃取线程池
 * 替代原有的 threadpool 类
 */
template <typename T>
class WorkStealingPool
{
private:
    int m_thread_number;                        // 线程数量
    int m_actor_model;                          // 模型切换 (0=Proactor, 1=Reactor)
    connection_pool* m_connPool;                // 数据库连接池

    std::vector<WorkerThread<T>*> workers;      // 工作线程对象数组
    std::vector<std::thread> threads;           // 线程对象数组
    std::atomic<bool> shutdown;                 // 关闭标志
    std::atomic<int> round_robin_counter;       // 轮询计数器

public:
    WorkStealingPool(int actor_model, connection_pool* connPool, int thread_number = 8)
        : m_thread_number(thread_number), m_actor_model(actor_model),
          m_connPool(connPool), shutdown(false), round_robin_counter(0)
    {
        if (thread_number <= 0 || thread_number > work_stealing_globals::MAX_WORKERS)
            throw std::exception();

        // 创建工作线程对象
        for (int i = 0; i < m_thread_number; ++i)
        {
            workers.push_back(new WorkerThread<T>(i, &workers, &shutdown));
        }

        // 启动线程
        for (int i = 0; i < m_thread_number; ++i)
        {
            threads.emplace_back([this, i]() {
                workers[i]->run();
            });
        }
    }

    ~WorkStealingPool()
    {
        // 设置关闭标志
        shutdown.store(true, std::memory_order_release);

        // 唤醒所有线程
        work_stealing_globals::g_sleeper_cv.notify_all();

        // 等待所有线程结束
        for (auto& t : threads)
        {
            if (t.joinable())
                t.join();
        }

        // 清理工作线程对象
        for (auto* w : workers)
        {
            delete w;
        }
    }

    // Reactor 模式：添加任务（带状态）
    bool append(T* request, int state)
    {
        if (!request)
            return false;

        // 创建任务
        IO_Task<T>* task = new IO_Task<T>(request, state, m_actor_model, m_connPool);

        // 使用轮询策略选择目标线程
        int target = round_robin_counter.fetch_add(1, std::memory_order_relaxed) % m_thread_number;

        // 将任务添加到目标线程的本地队列
        workers[target]->push_task(task);

        return true;
    }

    // Proactor 模式：添加任务（无状态）
    bool append_p(T* request)
    {
        if (!request)
            return false;

        // 创建任务
        IO_Task<T>* task = new IO_Task<T>(request, 0, m_actor_model, m_connPool);

        // 使用轮询策略选择目标线程
        int target = round_robin_counter.fetch_add(1, std::memory_order_relaxed) % m_thread_number;

        // 将任务添加到目标线程的本地队列
        workers[target]->push_task(task);

        return true;
    }
};

// 任务执行的实现（放在类外，避免循环依赖）
template <typename T>
void IO_Task<T>::execute()
{
    if (!request)
        return;

    if (actor_model == 1)
    {
        // Reactor 模式
        if (state == 0)
        {
            // 读操作
            if (request->read_once())
            {
                request->improv = 1;
                connectionRAII mysqlcon(&request->mysql, connPool);
                request->process();
            }
            else
            {
                request->improv = 1;
                request->timer_flag = 1;
            }
        }
        else
        {
            // 写操作
            if (request->write())
            {
                request->improv = 1;
            }
            else
            {
                request->improv = 1;
                request->timer_flag = 1;
            }
        }
    }
    else
    {
        // Proactor 模式
        connectionRAII mysqlcon(&request->mysql, connPool);
        request->process();
    }
}

#endif // WORK_STEALING_POOL_H
