# 协程+线程池混合架构集成指南

## 📋 概述

本文档说明如何将协程调度器、CPU线程池、任务分发器集成到现有的HTTP处理流程中。

---

## 🏗️ 架构组件

### 已实现的组件

1. **coroutine/advanced_scheduler.h** - 增强型协程调度器
2. **coroutine/thread_pool_bridge.h** - 协程-线程池桥接
3. **threadpool/cpu_thread_pool.h** - CPU任务线程池
4. **scheduler/task_dispatcher.h** - 智能任务分发器
5. **coroutine/cpu_task.h** - CPU任务协程包装器

### 组件关系图

```
HTTP请求 → TaskDispatcher → 判断任务类型
                │
        ┌───────┴────────┐
        │                │
    I/O任务          CPU任务
        │                │
  AdvancedScheduler  CpuThreadPool
        │                │
   io_uring协程     线程池执行
        │                │
    ←───┴────────────────┘
         返回响应
```

---

## 🔧 集成步骤

### Step 1: 在webserver.h中添加成员变量

```cpp
// webserver.h

#ifdef USE_COROUTINE
#include "coroutine/advanced_scheduler.h"
#include "threadpool/cpu_thread_pool.h"
#include "scheduler/task_dispatcher.h"
#endif

class WebServer {
private:
#ifdef USE_COROUTINE
    // 协程调度器（替代原有的CoroScheduler）
    std::unique_ptr<AdvancedScheduler> m_coro_scheduler;

    // CPU线程池
    std::unique_ptr<CpuThreadPool> m_cpu_thread_pool;

    // 任务分发器
    std::unique_ptr<TaskDispatcher> m_task_dispatcher;
#endif
};
```

### Step 2: 在WebServer::init()中初始化组件

```cpp
// webserver.cpp - WebServer::init()

#ifdef USE_COROUTINE
if (m_event_loop_mode == COROUTINE_MODE) {
    // 创建CPU线程池（线程数=CPU核心数的一半，避免过度竞争）
    size_t cpu_threads = std::thread::hardware_concurrency() / 2;
    if (cpu_threads < 2) cpu_threads = 2;

    m_cpu_thread_pool = std::make_unique<CpuThreadPool>(cpu_threads, 1000);
    LOG_INFO("CPU thread pool created: %zu threads", cpu_threads);

    // 创建任务分发器
    m_task_dispatcher = std::make_unique<TaskDispatcher>();
    LOG_INFO("Task dispatcher created");

    // 创建增强型协程调度器（最多10000个并发协程）
    m_coro_scheduler = std::make_unique<AdvancedScheduler>(
        m_io_uring_manager.get(), 10000);
    LOG_INFO("Advanced coroutine scheduler created");
}
#endif
```

### Step 3: 修改协程事件循环（使用AdvancedScheduler）

```cpp
// webserver.cpp - eventLoop_coro()

#ifdef USE_COROUTINE
void WebServer::eventLoop_coro() {
    if (!m_io_uring_manager || !m_io_uring_manager->is_initialized()) {
        LOG_ERROR("io_uring not initialized");
        return;
    }

    if (!m_coro_scheduler) {
        LOG_ERROR("Advanced scheduler not initialized");
        return;
    }

    LOG_INFO("Starting advanced coroutine event loop");

    // 启动 accept 协程
    m_coro_scheduler->spawn(
        accept_connections_coro(m_coro_scheduler.get()),
        CoroutinePriority::HIGH,  // accept是高优先级
        "accept-loop"
    );

    // 运行调度器
    m_coro_scheduler->run();

    // 打印统计信息
    m_coro_scheduler->print_stats();
    m_cpu_thread_pool->print_stats();
    m_task_dispatcher->print_stats();

    LOG_INFO("Advanced coroutine event loop stopped");
}
#endif
```

### Step 4: 实现HTTP连接处理协程（集成所有组件）

```cpp
// webserver.cpp - handle_http_connection_coro()

#ifdef USE_COROUTINE
Task<void> WebServer::handle_http_connection_coro(
    int connfd,
    struct sockaddr_in client_address
) {
    LOG_DEBUG("Coroutine: Handling connection fd=%d", connfd);

    // 分配缓冲区
    const size_t BUFFER_SIZE = 2048;
    auto read_buffer = std::make_shared<std::string>(BUFFER_SIZE, '\0');

    try {
        while (true) {
            // 1. 异步读取HTTP请求（I/O操作，使用协程）
            ssize_t bytes_read = co_await async_read(
                m_io_uring_manager.get(), connfd,
                &(*read_buffer)[0], BUFFER_SIZE
            );

            if (bytes_read <= 0) {
                LOG_DEBUG("Connection closed: fd=%d", connfd);
                break;
            }

            const char* request_line = read_buffer->c_str();
            LOG_DEBUG("Received request: %.100s", request_line);

            // 2. 使用任务分发器判断请求类型
            TaskType task_type = m_task_dispatcher->classify_request(request_line);

            if (task_type == TaskType::CPU_INTENSIVE) {
                // ========== CPU密集型请求 ==========
                LOG_INFO("CPU task detected, routing to thread pool");

                // 解析CPU任务参数（如：/cpu_compute?task=primes&level=3）
                char task_name[32];
                int level = 3;

                if (parse_cpu_task_params(request_line, task_name, sizeof(task_name), &level)) {
                    // 在线程池执行CPU计算（不阻塞协程）
                    std::string result = co_await dispatch_cpu_task(
                        task_name, level, m_io_uring_manager.get()
                    );

                    // 构造HTTP响应
                    std::string response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(result.size()) + "\r\n"
                        "Connection: close\r\n"
                        "\r\n" + result;

                    // 异步写入响应
                    co_await async_write(
                        m_io_uring_manager.get(), connfd,
                        response.c_str(), response.size()
                    );

                    LOG_INFO("CPU task completed: %s level=%d", task_name, level);
                } else {
                    // 参数解析失败
                    const char* error_response =
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 30\r\n"
                        "\r\n"
                        "Invalid CPU task parameters\n";
                    co_await async_write(m_io_uring_manager.get(), connfd,
                                         error_response, strlen(error_response));
                }

                break;  // 关闭连接

            } else if (task_type == TaskType::IO_INTENSIVE) {
                // ========== I/O密集型请求 ==========
                LOG_DEBUG("I/O task detected, handling with coroutine");

                // 检查文件上传
                if (strstr(request_line, "POST /upload") != nullptr) {
                    // 处理文件上传（已有实现）
                    // ...
                    break;
                }

                // 检查文件下载
                if (strstr(request_line, "GET /download/") != nullptr) {
                    // 处理文件下载（已有实现）
                    // ...
                    break;
                }

                // 检查API请求
                if (strstr(request_line, "GET /api/") != nullptr) {
                    // 处理API请求（已有实现）
                    // ...
                    break;
                }

                // 默认静态文件处理
                // ...
                break;

            } else {
                // ========== 混合型/未知类型 ==========
                LOG_DEBUG("Mixed/Unknown task, using default handler");

                // 使用默认处理逻辑
                // ...
                break;
            }
        }
    } catch (const IoError& e) {
        LOG_ERROR("I/O error on fd=%d: %s", connfd, e.what());
    } catch (const std::exception& e) {
        LOG_ERROR("Exception on fd=%d: %s", connfd, e.what());
    }

    // 关闭连接
    close(connfd);
    LOG_DEBUG("Coroutine: Connection closed fd=%d", connfd);
}
#endif
```

### Step 5: 修改accept协程（使用AdvancedScheduler）

```cpp
// webserver.cpp - accept_connections_coro()

#ifdef USE_COROUTINE
Task<void> WebServer::accept_connections_coro(AdvancedScheduler* scheduler) {
    LOG_INFO("Accept coroutine started");

    while (true) {
        struct sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);

        // 异步accept
        int connfd = co_await async_accept(
            m_io_uring_manager.get(),
            m_listenfd,
            (struct sockaddr*)&client_address,
            &client_addrlen
        );

        if (connfd < 0) {
            LOG_ERROR("Accept failed: %d", connfd);
            continue;
        }

        LOG_DEBUG("Accepted connection: fd=%d from %s:%d",
                  connfd,
                  inet_ntoa(client_address.sin_addr),
                  ntohs(client_address.sin_port));

        // 为每个连接创建一个新协程（NORMAL优先级）
        scheduler->spawn(
            handle_http_connection_coro(connfd, client_address),
            CoroutinePriority::NORMAL,
            "http-handler"
        );
    }
}
#endif
```

---

## 📊 完整示例：处理混合请求

```cpp
// 示例：处理一个需要先查询数据库（I/O），再进行数据分析（CPU）的请求

Task<void> handle_analytics_request(int connfd, IoUringManager* io_mgr) {
    // 1. I/O操作：读取请求
    auto request = co_await async_read(io_mgr, connfd, ...);

    // 2. I/O操作：查询数据库
    auto db_data = co_await query_database_async(request.user_id);

    // 3. CPU密集操作：数据分析（在线程池执行）
    auto analysis_result = co_await run_cpu_task<std::string>([db_data]() {
        // 复杂的数据分析逻辑
        return analyze_user_behavior(db_data);
    }, io_mgr);

    // 4. I/O操作：写入响应
    co_await async_write(io_mgr, connfd, analysis_result.c_str(), ...);

    close(connfd);
}
```

---

## ⚙️ 配置建议

### CPU线程池配置

```cpp
// 推荐配置
size_t cpu_threads = std::thread::hardware_concurrency() / 2;
size_t max_queue = 10000;

// 场景1：CPU密集型服务器
cpu_threads = std::thread::hardware_concurrency();  // 全部核心

// 场景2：I/O为主，偶尔CPU计算
cpu_threads = std::thread::hardware_concurrency() / 4;  // 1/4核心

// 场景3：均衡
cpu_threads = std::thread::hardware_concurrency() / 2;  // 1/2核心（推荐）
```

### 协程调度器配置

```cpp
// 最大并发协程数
size_t max_coroutines = 10000;  // 根据内存调整

// 场景1：内存受限
max_coroutines = 1000;

// 场景2：高并发
max_coroutines = 50000;
```

---

## 🐛 调试技巧

### 1. 启用详细日志

```cpp
// 在main.cpp中设置日志级别
Log::get_instance()->init("./server.log", 2048, 800000, 8, LOG_DEBUG);
```

### 2. 打印统计信息

```cpp
// 定期打印（如每10秒）
void print_system_stats() {
    m_coro_scheduler->print_stats();
    m_cpu_thread_pool->print_stats();
    m_task_dispatcher->print_stats();
}
```

### 3. 监控协程泄漏

```cpp
// 检查是否有协程未完成
size_t active = m_coro_scheduler->active_task_count();
if (active > expected_value) {
    LOG_WARN("Possible coroutine leak: %zu active tasks", active);
}
```

---

## 📈 性能优化建议

### 1. 避免在协程中执行CPU密集操作

```cpp
// ❌ 错误：阻塞协程调度器
Task<void> bad_handler() {
    auto result = heavy_cpu_computation();  // 阻塞！
    co_await async_write(...);
}

// ✅ 正确：使用线程池
Task<void> good_handler() {
    auto result = co_await run_cpu_task<std::string>([]() {
        return heavy_cpu_computation();
    }, io_mgr);
    co_await async_write(...);
}
```

### 2. 批量处理I/O操作

```cpp
// 并发发起多个I/O请求（如果可能）
Task<void> parallel_io() {
    auto task1 = async_read(...);
    auto task2 = async_read(...);

    auto result1 = co_await task1;
    auto result2 = co_await task2;
}
```

### 3. 复用缓冲区

```cpp
// 使用shared_ptr确保缓冲区生命周期
auto buffer = std::make_shared<std::string>(4096, '\0');
co_await async_read(io_mgr, fd, &(*buffer)[0], buffer->size());
```

---

## ✅ 测试清单

- [ ] CPU任务能够在线程池执行（检查日志）
- [ ] I/O任务使用协程处理（不阻塞）
- [ ] 协程数量在合理范围内（不泄漏）
- [ ] CPU线程池队列不溢出
- [ ] 任务分发器正确识别请求类型
- [ ] 统计信息准确
- [ ] 压力测试下系统稳定

---

## 🔗 相关文档

- `COROUTINE_GUIDE.md` - 协程基础
- `CPU_INTENSIVE_IMPLEMENTATION.md` - CPU任务实现
- `docs/COROUTINE_OPTIMIZATION_DESIGN.md` - 设计原理

---

## 🎯 总结

通过集成协程调度器、CPU线程池和任务分发器，我们实现了：

✅ I/O操作完全异步（协程）
✅ CPU计算不阻塞I/O（线程池）
✅ 自动任务分发（智能路由）
✅ 统一的协程API（代码简洁）
✅ 高性能和可维护性兼得

**预期性能提升**：
- 混合场景 QPS: **+30-40%**
- CPU利用率: **+25%**
- 代码可读性: **显著提升**
