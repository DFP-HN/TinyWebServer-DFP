# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TinyWebServer is a lightweight C++ web server for Linux that implements a high-performance concurrent model. This is an educational project designed to help beginners learn network programming.

**⚠️ IMPORTANT: This codebase has been refactored to remove tight coupling and static dependencies. See the "Refactored Architecture" section below.**

**Key Features:**
- Thread pool + non-blocking socket + epoll (ET and LT modes) + event handling (Reactor and simulated Proactor)
- State machine for HTTP request parsing (supports GET and POST)
- MySQL database integration for user registration/login
- Synchronous/asynchronous logging system
- Database connection pool with RAII
- Timer-based inactive connection cleanup
- **NEW**: Dependency injection pattern for decoupled components
- **NEW**: EpollManager and UserManager for separation of concerns
- Capable of handling 10,000+ concurrent connections (tested with Webbench)

**Recommended Reading:** The project is based on "Linux High Performance Server Programming" by You Shuang.

## Refactored Architecture

### ⭐ Key Changes from Original Design

This codebase has been **decoupled and refactored** to eliminate static dependencies and global state. The main improvements are:

#### 1. New Components

**EpollManager (`epoll/epoll_manager.h/cpp`)**
- Encapsulates all epoll operations
- Eliminates global `addfd()`, `removefd()`, `modfd()` functions
- Provides clean interface for epoll management

**UserManager (`user/user_manager.h/cpp`)**
- Manages user data and connection counting
- Replaces `http_conn::m_user_count` static member
- Replaces global `users` map and `m_lock` variables
- Thread-safe user operations

#### 2. Removed Static Dependencies

**Before (Tightly Coupled)**:
```cpp
class http_conn {
    static int m_epollfd;      // Global epoll fd
    static int m_user_count;   // Global user count
};

class Utils {
    static int *u_pipefd;      // Global pipe fd
    static int u_epollfd;      // Global epoll fd
};

// Global variables in http_conn.cpp
locker m_lock;
map<string, string> users;
```

**After (Decoupled)**:
```cpp
class http_conn {
    // Static members removed
    // Dependencies injected via setters
    void set_epoll_manager(EpollManager *epoll_mgr);
    void set_user_manager(UserManager *user_mgr);
private:
    EpollManager *m_epoll_manager;
    UserManager *m_user_manager;
};

class Utils {
    // Static members removed
    void set_epoll_manager(EpollManager *epoll_mgr);
    void set_signal_pipe(int *pipefd);
private:
    EpollManager *m_epoll_manager;
    int *m_pipefd;
};
```

#### 3. Benefits of Refactoring

- ✅ **Testability**: Can easily mock dependencies
- ✅ **Maintainability**: Explicit dependencies, clearer code flow
- ✅ **Extensibility**: Supports multiple server instances
- ✅ **Thread Safety**: Centralized synchronization in UserManager
- ✅ **No Global State**: All state is encapsulated in objects

### Component Structure

The codebase is organized into modular components:

- **`webserver.h/cpp`**: Main server class with injected dependencies
- **`config.h/cpp`**: Command-line argument parser
- **`main.cpp`**: Entry point - creates managers and injects dependencies
- **`epoll/`**: **NEW** - EpollManager for epoll operations
- **`user/`**: **NEW** - UserManager for user data and connection counting
- **`threadpool/`**: Half-sync/half-reactive thread pool implementation (header-only template)
- **`http/`**: HTTP connection handler with state machine parser (refactored to use DI)
- **`CGImysql/`**: Database connection pool with RAII wrapper (`connectionRAII`)
- **`log/`**: Logging system with blocking queue for async mode
- **`timer/`**: Sorted linked list timer with dependency injection (refactored)
- **`lock/`**: POSIX synchronization wrapper classes (sem, locker, cond)
- **`root/`**: Static web resources (HTML, images, videos)
- **`test_pressure/`**: Webbench stress testing tool

### Initialization Pattern

**Key Difference**: Dependencies must be injected before use

```cpp
// 1. Create manager instances
EpollManager epoll_manager;
UserManager user_manager;

// 2. Create WebServer
WebServer server;
server.m_epoll_manager = &epoll_manager;
server.m_user_manager = &user_manager;

// 3. Inject dependencies into all http_conn instances
for (int i = 0; i < MAX_FD; ++i) {
    server.users[i].set_epoll_manager(&epoll_manager);
    server.users[i].set_user_manager(&user_manager);
}

// 4. Configure Utils
server.utils.set_epoll_manager(&epoll_manager);
server.utils.set_signal_pipe(server.m_pipefd);
set_global_utils_instance(&server.utils);

// 5. Normal initialization
server.init(...);
server.eventListen();
server.eventLoop();
```

## Build Commands

### Quick Start with Docker (推荐)

使用 Docker Compose 一键启动（包含 MySQL）：

```bash
# 启动所有服务
docker-compose up -d

# 查看运行状态
docker-compose ps

# 访问服务
# http://localhost:9006
```

详见 `DOCKER.md` 获取完整 Docker 部署指南。

### 本地编译运行

#### Build the server
```bash
sh ./build.sh
# Or directly:
make server
```

#### Clean build artifacts
```bash
make clean
```

#### Run the server
```bash
./server
```

The server runs on port 9006 by default. Access via browser at `ip:9006`.

## Database Setup

Before running, set up MySQL database:

```sql
-- Create database
create database yourdb;

-- Create user table
USE yourdb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;

-- Add test data
INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```

Then modify database credentials in `main.cpp:6-8`:
```cpp
string user = "root";
string passwd = "root";
string databasename = "yourdb";
```

## Command Line Options

The server accepts various runtime configuration options:

```bash
./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]
```

- **-p**: Port number (default: 9006)
- **-l**: Log write mode: 0=sync, 1=async (default: 0)
- **-m**: Trigger mode: 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET (default: 0)
- **-o**: Graceful connection close: 0=off, 1=on (default: 0)
- **-s**: Database connection pool size (default: 8)
- **-t**: Thread pool size (default: 8)
- **-c**: Log switch: 0=on, 1=off (default: 0)
- **-a**: Reactor model: 0=Proactor, 1=Reactor (default: 0)

Example:
```bash
./server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1
```
This runs on port 9007 with async logging, LT+LT mode, graceful close, 10 DB connections, 10 threads, logs off, and Reactor model.

## Architecture Overview

### Component Structure

The codebase is organized into modular components:

- **`webserver.h/cpp`**: Main server class that orchestrates all components
- **`config.h/cpp`**: Command-line argument parser
- **`main.cpp`**: Entry point - initializes and runs WebServer
- **`threadpool/`**: Half-sync/half-reactive thread pool implementation (header-only template)
- **`http/`**: HTTP connection handler with state machine parser
- **`CGImysql/`**: Database connection pool with RAII wrapper (`connectionRAII`)
- **`log/`**: Logging system with blocking queue for async mode
- **`timer/`**: Sorted linked list timer for handling inactive connections
- **`lock/`**: POSIX synchronization wrapper classes (sem, locker, cond)
- **`root/`**: Static web resources (HTML, images, videos)
- **`test_pressure/`**: Webbench stress testing tool

### Concurrency Model

**Two Modes Available:**

1. **Proactor (default, `-a 0`)**: Main thread handles I/O, worker threads process logic
   - Main thread reads/writes data via epoll
   - Worker threads process HTTP requests and generate responses
   - Better performance in most tests (~93k-97k QPS)

2. **Reactor (`-a 1`)**: Worker threads handle both I/O and logic
   - Main thread only monitors events
   - Worker threads read, process, and write
   - Slightly lower performance (~69k QPS with LT+ET)

**Trigger Modes:**
- **LT (Level Triggered)**: Traditional epoll behavior, easier to use
- **ET (Edge Triggered)**: More efficient but requires careful handling of partial reads/writes

### Thread Pool Implementation

Template-based thread pool (`threadpool<T>`) in `threadpool/threadpool.h`:
- Worker threads continuously fetch tasks from a shared queue
- Uses semaphore (`m_queuestat`) for signaling and mutex (`m_queuelocker`) for protection
- `append()` for Reactor mode (passes read/write state)
- `append_p()` for Proactor mode (task already has data)
- Threads are detached and run until program exit

### HTTP Processing

State machine in `http/http_conn.cpp` with three states:
1. **CHECK_STATE_REQUESTLINE**: Parse request line (method, URL, version)
2. **CHECK_STATE_HEADER**: Parse headers
3. **CHECK_STATE_CONTENT**: Parse body (for POST)

Returns HTTP_CODE enum to indicate result (NO_REQUEST, GET_REQUEST, BAD_REQUEST, etc.)

POST requests trigger user authentication against MySQL database using connection pool.

### Database Connection Pool

Singleton pattern (`connection_pool::GetInstance()`) managing MySQL connections:
- Pre-creates connections in `init()`
- `GetConnection()` uses semaphore to block when pool exhausted
- `ReleaseConnection()` returns connection to pool
- **RAII wrapper `connectionRAII`** automatically acquires/releases connections

### Logging System

Singleton `Log` class with two modes:
- **Synchronous**: Direct write to file (thread-safe with mutex)
- **Asynchronous**: Writes to blocking queue, dedicated thread flushes to disk
- Macros: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`
- Automatically rotates by day and line count

### Timer Management

Sorted doubly-linked list of timers (`sort_timer_lst`) in `timer/lst_timer.cpp`:
- Each connection has a timer; inactive connections trigger callbacks
- `tick()` called periodically via `SIGALRM` signal
- Callback closes connection and removes from epoll
- Timers adjusted on each successful read/write

### Event Loop

Main event loop in `webserver.cpp:eventLoop()`:
1. `epoll_wait()` for events
2. New connections → `dealclientdata()` → create timer
3. Signal events → `dealwithsignal()` → handle timeout/shutdown
4. Read events → `dealwithread()` → append to thread pool
5. Write events → `dealwithwrite()` → append to thread pool
6. Process timer tick on timeout

## Development Notes

- **Platform**: Linux only (uses POSIX APIs and Linux-specific system calls)
- **Dependencies**: pthread, MySQL client library (`-lpthread -lmysqlclient`)
- **Compiler**: g++ (C++98/C++03 compatible, uses some C++11 features like local static singletons)
- **Test Environment**: Originally developed on Ubuntu 16.04 with MySQL 5.7.29
- **Windows**: Not supported (uses Unix domain sockets, epoll, POSIX signals)

### Important: Refactored Codebase

**⚠️ This codebase uses dependency injection**. When working with the code:

1. **Never add static members** to `http_conn`, `Utils`, or other core classes
2. **Always inject dependencies** through constructor or setter methods
3. **Use EpollManager** instead of global `addfd()`, `removefd()`, `modfd()`
4. **Use UserManager** for user data and connection counting
5. **Backup files** are available with `.bak` extension

### Code Style Notes

- Uses mix of C++98 and C++11 features
- Dependency injection pattern for core components
- Thread synchronization wrappers throw `std::exception()` on failure
- State machines extensively used for parsing
- RAII pattern for resource management (database connections, memory mapping)

### Refactoring Documentation

For detailed information about the refactoring:
- **`REFACTORING.md`**: Complete refactoring design and rationale
- **`IMPLEMENTATION_STATUS.md`**: Current implementation status and TODOs
- **Original code**: Available in `.bak` files for reference

### Key Files to Understand

1. **`epoll/epoll_manager.h`**: All epoll operations (http/http_conn.h:8)

### Static Resources

The `root/` directory contains all web assets served by the server:
- `welcome.html`: Home page
- `register.html`, `log.html`: User registration/login forms
- `picture.html`, `video.html`: Media request pages
- Images and videos for demonstration

### Performance Testing

Use Webbench for stress testing:
```bash
cd test_pressure/webbench-1.5
# Build webbench if needed
# Run pressure test (example: 10000 clients, 5 seconds)
./webbench -c 10000 -t 5 http://localhost:9006/
```

Close logging (`-c 1`) for more accurate performance measurement.

## Docker Deployment

### Prerequisites
- Docker Engine 20.10+
- Docker Compose 1.29+

### Quick Start
```bash
# 启动服务（包含 MySQL）
docker-compose up -d

# 查看日志
docker-compose logs -f webserver

# 停止服务
docker-compose down
```

### Docker Files
- **`Dockerfile`**: 应用容器定义
- **`docker-compose.yml`**: 多容器编排配置
- **`docker-entrypoint.sh`**: 容器启动脚本
- **`docker/init-db/init.sql`**: 数据库初始化脚本
- **`DOCKER.md`**: 完整 Docker 部署文档

### Benefits
- ✅ 无需手动安装依赖
- ✅ 一键启动完整环境（包括 MySQL）
- ✅ 自动初始化数据库
- ✅ 跨平台支持（Linux/Mac/Windows）
- ✅ 环境隔离，避免冲突

详见 `DOCKER.md` 获取详细说明和故障排查。

## Common Issues

### Docker Environment
- **端口占用**: 修改 `docker-compose.yml` 中的端口映射
- **MySQL 连接失败**: 检查 `docker-compose logs mysql`
- **容器无法启动**: 查看 `docker-compose logs webserver`

### Native Environment
- **Webbench not found**: Delete the `webbench` executable and recompile
- **MySQL connection failures**: Verify database credentials in `main.cpp` and ensure MySQL is running
- **Port already in use**: Change port with `-p` flag
- **Build errors**: Ensure MySQL development headers are installed (`libmysqlclient-dev`)
