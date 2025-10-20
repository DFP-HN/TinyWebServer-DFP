# 异步IO + 零拷贝 + 协程 优化设计方案

## 1. 概述

本文档描述 TinyWebServer 的三大核心优化方向：
1. **零拷贝优化**：完善 sendfile 实现，添加 splice 支持
2. **io_uring 异步 I/O**：引入 Linux 最新的高性能异步 I/O 框架
3. **C++20 协程**：使用协程简化异步编程模型

## 2. 当前架构分析

### 2.1 现有 I/O 模型
- **基础模型**: epoll + 线程池
- **工作模式**: Reactor/Proactor 两种模式
- **I/O 操作**: 传统的 read()/write() 系统调用
- **已有优化**: 部分 sendfile 零拷贝实现（http_conn.cpp:734-800）

### 2.2 现有优势
✅ 依赖注入架构（EpollManager, UserManager）
✅ 智能指针 + RAII 资源管理
✅ 静态文件缓存系统
✅ 批处理任务队列（减少锁竞争）
✅ 部分零拷贝支持（sendfile）

### 2.3 改进空间
❌ sendfile 使用场景有限（仅大文件）
❌ 小文件仍使用传统 I/O
❌ 缺乏真正的异步 I/O（仍是同步阻塞模型）
❌ 线程池模型在高并发下有上下文切换开销
❌ 代码流程复杂（状态机 + 回调）

---

## 3. 零拷贝优化方案

### 3.1 优化目标
- 减少 CPU 拷贝次数，降低 CPU 使用率
- 提升静态文件传输性能
- 降低内存带宽压力

### 3.2 技术选型

#### 3.2.1 sendfile() - 已部分实现
**用途**: 文件 → Socket 的零拷贝传输

```cpp
// 当前实现位置: http_conn.cpp:767
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
```

**优化点**:
1. 扩展到更多文件类型（当前主要用于大文件）
2. 添加小文件优化策略（< 64KB 使用缓存，64KB-4MB 使用 sendfile）
3. 支持 Range 请求的零拷贝

#### 3.2.2 splice() - 新增
**用途**: 两个文件描述符之间的零拷贝传输（通过管道）

```cpp
// 应用场景: CGI 输出、代理转发
ssize_t splice(int fd_in, loff_t *off_in, int fd_out,
               loff_t *off_out, size_t len, unsigned int flags);
```

**实现计划**:
```cpp
class ZeroCopyHelper {
public:
    // sendfile 封装（文件 → Socket）
    static ssize_t send_file(int sockfd, int filefd, off_t offset, size_t count);

    // splice 封装（管道传输）
    static ssize_t splice_transfer(int in_fd, int out_fd, size_t len);

    // 智能选择最佳传输方式
    enum TransferMethod {
        USE_SENDFILE,   // 大文件
        USE_CACHE,      // 小文件（内存缓存）
        USE_WRITEV,     // 分散写（头 + 体）
        USE_SPLICE      // 管道传输
    };

    static TransferMethod choose_method(size_t file_size, bool is_cached);
};
```

#### 3.2.3 mmap() - 保留现有实现
**用途**: 文件映射到内存（已在 http_conn.cpp 中使用）

**改进方向**:
- 小文件（< 64KB）: 使用缓存，避免 mmap 开销
- 中等文件（64KB - 4MB）: 使用 sendfile 零拷贝
- 大文件（> 4MB）: 使用 mmap + 分块传输

### 3.3 实现规划

#### 阶段 1: 完善 sendfile 策略
```cpp
// 新增: http/zero_copy.h/cpp
class ZeroCopyManager {
private:
    static const size_t SMALL_FILE_THRESHOLD = 64 * 1024;      // 64KB
    static const size_t LARGE_FILE_THRESHOLD = 4 * 1024 * 1024; // 4MB

public:
    // 根据文件大小和缓存状态选择传输方式
    bool send_response(http_conn *conn);
};
```

#### 阶段 2: 添加 splice 支持
```cpp
// 用于 CGI 输出传输
class SpliceTransfer {
private:
    int m_pipefd[2];  // 管道文件描述符

public:
    SpliceTransfer();
    ~SpliceTransfer();

    // CGI 输出 → Socket（零拷贝）
    ssize_t transfer_cgi_output(int cgi_fd, int sockfd, size_t len);
};
```

#### 阶段 3: 优化小文件处理
- 集成现有的 StaticCache 系统
- 小文件直接从内存发送（已缓存）
- 避免不必要的系统调用开销

---

## 4. io_uring 异步 I/O 方案

### 4.1 为什么选择 io_uring？

| 特性 | epoll | io_uring |
|-----|-------|----------|
| 系统调用次数 | 每次事件需要系统调用 | 批量提交，减少系统调用 |
| 真正异步 | 否（仍需 read/write） | 是（内核完成 I/O） |
| 零拷贝支持 | 有限 | 原生支持（registered buffers） |
| CPU 效率 | 中等 | 高（轮询模式可达 100% CPU 利用率） |
| 内核支持 | Linux 2.6+ | Linux 5.1+ |

**结论**: io_uring 是 Linux 未来的 I/O 方向，性能优于 epoll。

### 4.2 架构设计

#### 4.2.1 核心组件
```cpp
// io_uring/io_uring_manager.h
class IoUringManager {
private:
    struct io_uring m_ring;           // io_uring 实例
    int m_ring_fd;                     // ring 文件描述符
    unsigned m_entries;                // SQ/CQ 队列深度

public:
    IoUringManager(unsigned entries = 256);
    ~IoUringManager();

    // 初始化 io_uring
    bool init();

    // 提交异步读操作
    bool submit_read(int fd, void *buf, size_t len, uint64_t user_data);

    // 提交异步写操作
    bool submit_write(int fd, const void *buf, size_t len, uint64_t user_data);

    // 提交 sendfile 操作
    bool submit_sendfile(int out_fd, int in_fd, off_t offset, size_t len, uint64_t user_data);

    // 批量提交请求
    int submit_all();

    // 等待完成事件
    int wait_completions(struct io_uring_cqe **cqe_ptr, unsigned wait_nr = 1);

    // 处理完成队列
    void process_completions(std::function<void(struct io_uring_cqe*)> handler);
};
```

#### 4.2.2 事件循环重构
```cpp
// webserver.cpp 中的新事件循环
void WebServer::eventLoop_uring() {
    while (!stop_server) {
        // 批量提交所有待处理的 I/O 请求
        int submitted = m_io_uring_manager->submit_all();

        // 等待完成事件
        m_io_uring_manager->process_completions([this](struct io_uring_cqe *cqe) {
            // user_data 存储连接上下文
            http_conn *conn = reinterpret_cast<http_conn*>(io_uring_cqe_get_data(cqe));

            if (cqe->res < 0) {
                // I/O 错误处理
                handle_io_error(conn, cqe->res);
            } else {
                // I/O 成功处理
                handle_io_complete(conn, cqe->res);
            }

            // 标记 CQE 已处理
            io_uring_cqe_seen(&m_ring, cqe);
        });
    }
}
```

#### 4.2.3 HTTP 连接适配
```cpp
// http/http_conn.h 新增
class http_conn {
private:
    IoUringManager *m_io_uring_manager;  // 依赖注入

    // 异步 I/O 状态
    enum IoState {
        IO_IDLE,           // 空闲
        IO_READING,        // 正在读取
        IO_PROCESSING,     // 正在处理
        IO_WRITING,        // 正在写入
        IO_SENDFILE        // 正在发送文件
    };
    IoState m_io_state;

public:
    // 提交异步读请求
    bool submit_async_read();

    // 提交异步写请求
    bool submit_async_write();

    // 提交异步 sendfile
    bool submit_async_sendfile();

    // I/O 完成回调
    void on_read_complete(ssize_t nread);
    void on_write_complete(ssize_t nwritten);
    void on_sendfile_complete(ssize_t nsent);
};
```

### 4.3 实现路线图

#### 阶段 1: io_uring 基础封装（1-2天）
- [ ] 创建 IoUringManager 类
- [ ] 实现基本的读写操作
- [ ] 添加错误处理和日志

#### 阶段 2: 与现有架构集成（2-3天）
- [ ] 在 WebServer 中注入 IoUringManager
- [ ] 重构事件循环支持 io_uring
- [ ] 保留 epoll 作为后备模式（编译时选择）

#### 阶段 3: 高级特性（2-3天）
- [ ] registered buffers（固定内存，零拷贝）
- [ ] registered files（固定文件描述符，减少查表）
- [ ] 链式操作（一次提交多个操作）

#### 阶段 4: 性能优化（1-2天）
- [ ] 调整队列深度（SQ/CQ entries）
- [ ] 批量提交策略
- [ ] 轮询模式 vs 中断模式

---

## 5. C++20 协程方案

### 5.1 为什么使用协程？

**问题**:
- 当前的状态机代码复杂（CHECK_STATE_REQUESTLINE, CHECK_STATE_HEADER...）
- 回调地狱（异步 I/O 需要回调函数）
- 线程池上下文切换开销

**协程优势**:
```cpp
// 当前代码（状态机）
HTTP_CODE http_conn::process_read() {
    switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE:
            // 解析请求行
            break;
        case CHECK_STATE_HEADER:
            // 解析头部
            break;
        case CHECK_STATE_CONTENT:
            // 解析内容
            break;
    }
}

// 协程代码（顺序逻辑）
Task<HttpResponse> http_conn::handle_request_coro() {
    // 读取请求行
    auto request_line = co_await async_read_line();
    parse_request_line(request_line);

    // 读取头部
    while (true) {
        auto header = co_await async_read_line();
        if (header.empty()) break;
        parse_header(header);
    }

    // 读取内容
    if (m_content_length > 0) {
        auto body = co_await async_read_bytes(m_content_length);
        parse_content(body);
    }

    // 生成响应
    co_return generate_response();
}
```

### 5.2 协程框架设计

#### 5.2.1 核心组件
```cpp
// coroutine/task.h
template<typename T>
class Task {
public:
    struct promise_type {
        Task get_return_object();
        std::suspend_always initial_suspend();
        std::suspend_always final_suspend() noexcept;
        void unhandled_exception();
        void return_value(T value);
    };

    bool await_ready();
    void await_suspend(std::coroutine_handle<> handle);
    T await_resume();

private:
    std::coroutine_handle<promise_type> m_handle;
    T m_value;
};

// 协程调度器
class CoroScheduler {
private:
    std::vector<std::coroutine_handle<>> m_ready_queue;   // 就绪队列
    std::map<uint64_t, std::coroutine_handle<>> m_waiting_map;  // 等待 I/O 的协程

public:
    // 调度协程执行
    void schedule(std::coroutine_handle<> handle);

    // I/O 完成时恢复协程
    void resume_on_io_complete(uint64_t user_data);

    // 运行调度循环
    void run();
};
```

#### 5.2.2 异步原语
```cpp
// coroutine/async_io.h
class AsyncIO {
private:
    IoUringManager *m_io_uring;
    CoroScheduler *m_scheduler;

public:
    // 异步读（协程版本）
    Task<ssize_t> async_read(int fd, void *buf, size_t len) {
        uint64_t user_data = generate_unique_id();

        // 提交 io_uring 读请求
        m_io_uring->submit_read(fd, buf, len, user_data);

        // 挂起当前协程，等待 I/O 完成
        co_await IoAwaiter{m_scheduler, user_data};

        // I/O 完成后恢复，返回结果
        co_return get_io_result(user_data);
    }

    // 异步写
    Task<ssize_t> async_write(int fd, const void *buf, size_t len);

    // 异步 sendfile
    Task<ssize_t> async_sendfile(int out_fd, int in_fd, off_t offset, size_t len);
};
```

#### 5.2.3 HTTP 协程处理器
```cpp
// http/http_conn_coro.h
class HttpConnCoro : public http_conn {
private:
    AsyncIO *m_async_io;
    CoroScheduler *m_scheduler;

public:
    // 协程版本的请求处理
    Task<void> process_coro() {
        try {
            // 读取请求
            auto request = co_await read_http_request();

            // 处理请求
            auto response = co_await handle_request(request);

            // 发送响应
            if (response.has_file) {
                // 零拷贝发送文件
                co_await m_async_io->async_sendfile(
                    m_sockfd, response.file_fd, 0, response.file_size
                );
            } else {
                // 普通写操作
                co_await m_async_io->async_write(
                    m_sockfd, response.data, response.size
                );
            }
        } catch (const std::exception &e) {
            LOG_ERROR("Coroutine exception: %s", e.what());
            close_conn();
        }
    }

private:
    // 读取 HTTP 请求（协程版本）
    Task<HttpRequest> read_http_request() {
        HttpRequest req;

        // 读取请求行
        auto line = co_await read_line();
        parse_request_line(line, req);

        // 读取头部
        while (true) {
            auto header = co_await read_line();
            if (header.empty()) break;
            parse_header(header, req);
        }

        // 读取 body（如果有）
        if (req.content_length > 0) {
            req.body.resize(req.content_length);
            co_await m_async_io->async_read(
                m_sockfd, req.body.data(), req.content_length
            );
        }

        co_return req;
    }

    // 按行读取（协程版本）
    Task<std::string> read_line() {
        std::string line;
        while (true) {
            char ch;
            co_await m_async_io->async_read(m_sockfd, &ch, 1);
            if (ch == '\n') break;
            if (ch != '\r') line += ch;
        }
        co_return line;
    }
};
```

### 5.3 实现路线图

#### 阶段 1: 协程基础框架（2-3天）
- [ ] 实现 Task<T> 协程类型
- [ ] 实现 CoroScheduler 调度器
- [ ] 实现 IoAwaiter（等待 I/O 完成）

#### 阶段 2: 异步 I/O 集成（2-3天）
- [ ] AsyncIO 类封装 io_uring 操作
- [ ] 协程与 io_uring 事件循环集成
- [ ] 测试基本的读写协程

#### 阶段 3: HTTP 协程化（3-4天）
- [ ] 创建 HttpConnCoro 类
- [ ] 实现协程版本的 HTTP 解析
- [ ] 实现协程版本的响应发送
- [ ] 集成零拷贝（协程 + sendfile）

#### 阶段 4: 性能调优（2-3天）
- [ ] 协程池（复用协程栈）
- [ ] 批量调度优化
- [ ] 减少协程切换开销

---

## 6. 集成方案

### 6.1 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                        WebServer                             │
│  ┌────────────────────────────────────────────────────────┐ │
│  │                   Event Loop                            │ │
│  │  ┌──────────────┐       ┌──────────────┐              │ │
│  │  │   epoll      │  OR   │  io_uring    │              │ │
│  │  │  (传统模式)   │       │  (异步模式)   │              │ │
│  │  └──────────────┘       └──────────────┘              │ │
│  └────────────────────────────────────────────────────────┘ │
│                          │                                   │
│                          ▼                                   │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              Connection Handler                         │ │
│  │  ┌──────────────┐       ┌──────────────┐              │ │
│  │  │ http_conn    │  OR   │ HttpConnCoro │              │ │
│  │  │ (状态机模式)  │       │  (协程模式)   │              │ │
│  │  └──────────────┘       └──────────────┘              │ │
│  └────────────────────────────────────────────────────────┘ │
│                          │                                   │
│                          ▼                                   │
│  ┌────────────────────────────────────────────────────────┐ │
│  │               I/O Operations                            │ │
│  │  ┌──────────────┬──────────────┬──────────────┐        │ │
│  │  │ read/write   │  sendfile    │   splice     │        │ │
│  │  │  (传统I/O)   │  (零拷贝)    │  (零拷贝)    │        │ │
│  │  └──────────────┴──────────────┴──────────────┘        │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 编译时选择

```cpp
// config.h
#define USE_IO_URING 1      // 是否使用 io_uring
#define USE_COROUTINE 1     // 是否使用协程
#define USE_ZERO_COPY 1     // 是否使用零拷贝

// Makefile
ifeq ($(USE_IO_URING), 1)
    CXXFLAGS += -DUSE_IO_URING -luring
endif

ifeq ($(USE_COROUTINE), 1)
    CXXFLAGS += -DUSE_COROUTINE -std=c++20 -fcoroutines
endif
```

### 6.3 运行时配置

```bash
# 传统模式（epoll + 线程池）
./server -m 0

# io_uring 模式（异步 I/O）
./server -m 1 --io-uring

# 协程模式（io_uring + 协程）
./server -m 2 --io-uring --coroutine
```

---

## 7. 性能预期

### 7.1 基准测试指标

| 优化项 | QPS 提升 | CPU 降低 | 延迟降低 |
|-------|---------|---------|---------|
| 零拷贝（sendfile） | +10-15% | -20-30% | -5-10% |
| io_uring | +30-50% | -10-20% | -20-30% |
| 协程 | +20-30% | -15-25% | -10-15% |
| **综合优化** | **+60-100%** | **-40-50%** | **-30-40%** |

### 7.2 测试计划
1. Webbench 压测（静态文件）
2. wrk 压测（HTTP 接口）
3. 混合负载测试（数据库 + 静态文件）
4. 长连接压测（Keep-Alive）

---

## 8. 风险评估

### 8.1 技术风险
| 风险 | 影响 | 缓解措施 |
|-----|------|---------|
| io_uring 内核版本要求（5.1+） | 高 | 保留 epoll 后备模式 |
| C++20 编译器支持 | 中 | GCC 10+, Clang 10+ |
| 协程学习曲线 | 中 | 充分的文档和示例 |
| 性能回归风险 | 高 | 每个阶段做性能对比测试 |

### 8.2 实现风险
- **代码复杂度**: 协程和 io_uring 会增加代码复杂度
  - 缓解: 模块化设计，清晰的接口封装
- **调试难度**: 异步代码难以调试
  - 缓解: 完善的日志系统，单元测试覆盖

---

## 9. 时间规划

| 阶段 | 任务 | 工期 |
|-----|-----|------|
| **第1周** | 零拷贝优化完善 | 3-4天 |
|  | - 完善 sendfile 策略 | 1天 |
|  | - 添加 splice 支持 | 1天 |
|  | - 优化小文件处理 | 1天 |
|  | - 性能测试 | 0.5天 |
| **第2周** | io_uring 基础框架 | 5-6天 |
|  | - IoUringManager 实现 | 2天 |
|  | - 事件循环重构 | 2天 |
|  | - 高级特性（registered buffers） | 1天 |
|  | - 性能测试 | 1天 |
| **第3周** | 协程框架 | 5-6天 |
|  | - Task 和调度器 | 2天 |
|  | - AsyncIO 集成 | 2天 |
|  | - HttpConnCoro 实现 | 2天 |
| **第4周** | 集成和优化 | 4-5天 |
|  | - 完整集成测试 | 2天 |
|  | - 性能调优 | 2天 |
|  | - 文档和示例 | 1天 |

**总计**: 约 4 周（17-21 天）

---

## 10. 参考资料

### 10.1 io_uring
- [Efficient I/O with io_uring](https://kernel.dk/io_uring.pdf)
- [liburing Documentation](https://github.com/axboe/liburing)
- [io_uring 原理与实践](https://zhuanlan.zhihu.com/p/361955546)

### 10.2 零拷贝
- [Linux Zero Copy 技术](https://www.linuxjournal.com/article/6345)
- [sendfile vs splice](https://blog.csdn.net/dog250/article/details/78648907)

### 10.3 C++20 协程
- [C++ Coroutines: Understanding the Compiler Transform](https://lewissbaker.github.io/)
- [cppreference: Coroutines](https://en.cppreference.com/w/cpp/language/coroutines)
- [C++20 协程入门](https://zhuanlan.zhihu.com/p/363476100)

---

## 11. 下一步行动

1. ✅ 完成设计文档评审
2. ⏳ 零拷贝优化实现（第一优先级，改动最小）
3. ⏳ io_uring 框架搭建
4. ⏳ 协程框架实现
5. ⏳ 性能对比测试和报告
