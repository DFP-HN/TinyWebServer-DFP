# TinyWebServer 解耦重构方案

## 1. 原有架构的耦合问题

### 1.1 静态成员导致的紧耦合

**问题代码:**
```cpp
// http_conn.h
class http_conn {
public:
    static int m_epollfd;      // 所有http_conn共享同一个epoll fd
    static int m_user_count;   // 静态用户计数器
};

// timer/lst_timer.cpp
class Utils {
public:
    static int *u_pipefd;      // 静态管道fd
    static int u_epollfd;      // 静态epoll fd
};
```

**问题分析:**
- 全局状态难以测试和维护
- 违反单一职责原则
- 多个组件隐式依赖全局状态
- 无法创建多个独立的服务器实例

### 1.2 全局变量污染

**问题代码 (http_conn.cpp:17-18):**
```cpp
locker m_lock;
map<string, string> users;
```

**问题分析:**
- 全局变量在多线程环境下难以管理
- 命名空间污染
- 破坏封装性

### 1.3 循环依赖

**问题:**
- `timer/lst_timer.cpp` includes `http/http_conn.h`
- `cb_func()` 直接访问 `http_conn::m_user_count`

### 1.4 直接赋值静态成员

**问题代码 (webserver.cpp:143, 157-158):**
```cpp
http_conn::m_epollfd = m_epollfd;
Utils::u_pipefd = m_pipefd;
Utils::u_epollfd = m_epollfd;
```

## 2. 解耦重构方案

### 2.1 核心设计原则

1. **依赖注入 (Dependency Injection)**
   - 通过构造函数或setter方法注入依赖
   - 消除对静态成员和全局变量的依赖

2. **单一职责原则 (Single Responsibility Principle)**
   - 每个类只负责一个功能领域
   - 分离关注点

3. **依赖倒置原则 (Dependency Inversion Principle)**
   - 高层模块不应依赖低层模块
   - 两者都应依赖抽象

### 2.2 新增组件

#### 2.2.1 EpollManager (epoll/epoll_manager.h)

**职责:** 封装所有 epoll 相关操作

```cpp
class EpollManager {
public:
    bool create(int size = 5);
    int get_epollfd() const;
    int wait(epoll_event *events, int maxevents, int timeout);
    static int setnonblocking(int fd);
    bool addfd(int fd, bool one_shot, int trig_mode);
    bool removefd(int fd);
    bool modfd(int fd, int ev, int trig_mode);
};
```

**优点:**
- 集中管理 epoll 操作
- 消除全局 `addfd()`, `removefd()`, `modfd()` 函数
- 易于测试和模拟

#### 2.2.2 UserManager (user/user_manager.h)

**职责:** 管理用户数据和连接计数

```cpp
class UserManager {
public:
    int get_user_count() const;
    void increment_user_count();
    void decrement_user_count();

    bool find_user(const string &username, string &password) const;
    bool has_user(const string &username) const;
    bool add_user(const string &username, const string &password);

    locker &get_lock();
};
```

**优点:**
- 替代 `http_conn::m_user_count` 静态成员
- 替代全局 `users` map 和 `m_lock`
- 线程安全的用户管理
- 职责清晰

### 2.3 重构后的类

#### 2.3.1 http_conn_refactored.h

**主要改动:**
```cpp
class http_conn {
public:
    // 移除静态成员
    // static int m_epollfd;      // 已删除
    // static int m_user_count;   // 已删除

    // 添加依赖注入方法
    void set_epoll_manager(EpollManager *epoll_mgr);
    void set_user_manager(UserManager *user_mgr);

private:
    // 依赖注入的成员
    EpollManager *m_epoll_manager;
    UserManager *m_user_manager;
};
```

**使用方式:**
```cpp
http_conn conn;
conn.set_epoll_manager(&epoll_manager);
conn.set_user_manager(&user_manager);
conn.init(...);
```

#### 2.3.2 lst_timer_refactored.h

**主要改动:**
```cpp
// 回调函数现在接受依赖注入的参数
typedef void (*timer_callback)(client_data *user_data,
                               EpollManager *epoll_mgr,
                               UserManager *user_mgr);

class util_timer {
public:
    timer_callback cb_func;
    EpollManager *epoll_manager;
    UserManager *user_manager;
};

class Utils {
public:
    // 移除静态成员
    // static int *u_pipefd;   // 已删除
    // static int u_epollfd;   // 已删除

    // 添加依赖注入方法
    void set_epoll_manager(EpollManager *epoll_mgr);
    void set_signal_pipe(int *pipefd);

private:
    int *m_pipefd;
    EpollManager *m_epoll_manager;
};

// 重构后的回调函数
void cb_func_refactored(client_data *user_data,
                       EpollManager *epoll_mgr,
                       UserManager *user_mgr);
```

### 2.4 WebServer 初始化流程

**重构后的初始化:**
```cpp
WebServerRefactored server;

// 1. 创建管理器实例
EpollManager epoll_manager;
UserManager user_manager;

// 2. 初始化 epoll
epoll_manager.create(5);

// 3. 注入依赖到 WebServer
server.m_epoll_manager = &epoll_manager;
server.m_user_manager = &user_manager;

// 4. 初始化所有 http_conn 实例
for (int i = 0; i < MAX_FD; ++i) {
    server.users[i].set_epoll_manager(&epoll_manager);
    server.users[i].set_user_manager(&user_manager);
}

// 5. 初始化 Utils
server.utils.set_epoll_manager(&epoll_manager);
server.utils.set_signal_pipe(server.m_pipefd);

// 6. 正常启动服务器
server.init(...);
server.eventListen();
server.eventLoop();
```

## 3. 对比分析

### 3.1 耦合度对比

| 方面 | 原架构 | 重构后 |
|------|--------|--------|
| 静态依赖 | 5个静态成员变量 | 0个 |
| 全局变量 | 2个 (users, m_lock) | 0个 |
| 循环依赖 | timer → http_conn | 已消除 |
| 可测试性 | 困难（依赖全局状态） | 容易（依赖注入） |
| 多实例支持 | 不支持 | 支持 |

### 3.2 优点总结

#### 3.2.1 可测试性提升
- 可以轻松 mock EpollManager 和 UserManager
- 单元测试不需要全局状态初始化
- 可以并行测试多个实例

#### 3.2.2 可维护性提升
- 依赖关系显式化
- 职责划分清晰
- 易于理解代码流程

#### 3.2.3 可扩展性提升
- 可以创建多个独立的服务器实例
- 易于添加新的管理器组件
- 支持策略模式和工厂模式

#### 3.2.4 线程安全提升
- UserManager 封装了所有用户数据操作
- 锁的使用更加集中和可控
- 减少数据竞争风险

### 3.3 潜在缺点

#### 3.3.1 代码复杂度略有增加
- 需要手动注入依赖
- 类的数量增加

**缓解措施:**
- 使用工厂模式简化对象创建
- 提供辅助函数简化初始化流程

#### 3.3.2 轻微性能开销
- 函数调用通过指针间接访问
- 额外的指针存储

**影响评估:**
- 性能影响可忽略不计（现代CPU优化）
- 收益远大于代价

## 4. 迁移指南

### 4.1 逐步迁移策略

#### 阶段1: 共存阶段
- 保留原有代码
- 新增重构后的类（带 _refactored 后缀）
- 允许两套代码并存

#### 阶段2: 测试阶段
- 编写单元测试验证重构代码
- 性能对比测试
- 功能完整性测试

#### 阶段3: 替换阶段
- 逐步用重构代码替换原代码
- 删除 _refactored 后缀
- 更新文档

#### 阶段4: 清理阶段
- 删除旧代码
- 优化重构代码
- 完善文档

### 4.2 兼容性考虑

为了保持向后兼容，可以提供适配器类：

```cpp
class HttpConnAdapter {
public:
    static void init_static_deps(EpollManager *epoll, UserManager *user) {
        s_epoll = epoll;
        s_user = user;
    }

    static int get_epollfd() {
        return s_epoll ? s_epoll->get_epollfd() : -1;
    }

    static int get_user_count() {
        return s_user ? s_user->get_user_count() : 0;
    }

private:
    static EpollManager *s_epoll;
    static UserManager *s_user;
};
```

## 5. 使用示例

### 5.1 完整示例代码

参见 `examples/refactored_usage_example.cpp`

### 5.2 关键代码片段

```cpp
// 创建管理器
EpollManager epoll_mgr;
UserManager user_mgr;

// 初始化
epoll_mgr.create(5);

// 创建连接
http_conn conn;
conn.set_epoll_manager(&epoll_mgr);
conn.set_user_manager(&user_mgr);

// 使用连接
conn.init(sockfd, addr, root, trig_mode, close_log, user, passwd, dbname);

// 创建定时器
util_timer *timer = new util_timer;
timer->epoll_manager = &epoll_mgr;
timer->user_manager = &user_mgr;
timer->cb_func = cb_func_refactored;
```

## 6. 编译说明

### 6.1 Makefile 更新

```makefile
# 添加新的源文件
REFACTORED_SRCS = \
    epoll/epoll_manager.cpp \
    user/user_manager.cpp \
    timer/lst_timer_refactored.cpp

# 编译重构版本
server_refactored: main_refactored.cpp $(REFACTORED_SRCS) $(OTHER_SRCS)
	$(CXX) -o server_refactored $^ $(CXXFLAGS) -lpthread -lmysqlclient

# 编译原版本（保持兼容）
server: main.cpp $(ORIGINAL_SRCS)
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient
```

## 7. 总结

### 7.1 核心改进

1. **消除静态依赖**: 所有静态成员变量已移除
2. **消除全局变量**: 全局 users map 和 m_lock 已封装
3. **依赖注入**: 所有依赖通过构造函数或setter注入
4. **职责分离**: EpollManager 和 UserManager 各司其职

### 7.2 代码质量提升

- **耦合度**: 从高耦合降低到低耦合
- **内聚性**: 每个类职责单一，内聚性高
- **可测试性**: 从难以测试到易于测试
- **可维护性**: 代码结构清晰，易于理解

### 7.3 下一步建议

1. 编写完整的单元测试
2. 性能基准测试
3. 添加更多管理器（如 ConfigManager, LogManager）
4. 使用智能指针替代原始指针
5. 考虑引入依赖注入框架
