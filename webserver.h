#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <memory>  // 智能指针

#include "./http/http_conn.h"
#include "./user/user_manager.h"
#include "./timer/lst_timer.h"
#include "./cache/static_cache.h"
#include "./log/log.h"
#include "./io_uring/io_uring_manager.h"

#ifdef USE_COROUTINE
// 前向声明
template<typename T> class Task;
class CoroScheduler;
class AdvancedScheduler;
class CpuThreadPool;
class TaskDispatcher;
#endif

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 10;            //最小超时单位（优化为10秒，Keep-Alive超时为3*TIMESLOT=30秒）

// 事件循环模式
enum EventLoopMode {
    IO_URING_MODE = 1,    // io_uring 异步 I/O 模式（回调风格）
    COROUTINE_MODE = 2    // io_uring + 协程模式（C++20，推荐）
};

// 重构后的WebServer类
// 使用依赖注入，消除静态耦合
class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model, int event_loop_mode = COROUTINE_MODE);

    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();

    // io_uring 事件循环（回调风格）
    void eventLoop_uring();

#ifdef USE_COROUTINE
    // 协程事件循环
    void eventLoop_coro();

    // 协程函数
    Task<void> handle_http_connection_coro(int connfd, struct sockaddr_in client_address);
    Task<void> accept_connections_coro(AdvancedScheduler* scheduler);
#endif

    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(std::shared_ptr<util_timer> timer);
    void deal_timer(std::shared_ptr<util_timer> timer, int sockfd);

    // io_uring 专用方法（回调风格）
    void dealclientdata_uring();
    void dealwithread_uring(int sockfd);
    void dealwithwrite_uring(int sockfd);
    void handle_io_completion(struct io_uring_cqe *cqe);

public:
    //基础
    int m_port;
    std::unique_ptr<char[]> m_root;  // 智能指针管理
    int m_log_write;
    int m_close_log;
    int m_actormodel;
    int m_event_loop_mode;  // 事件循环模式（epoll 或 io_uring）

    int m_pipefd[2];
    std::unique_ptr<http_conn[]> users;  // 智能指针管理数组

    //数据库相关
    connection_pool *m_connPool;  // 单例，不需要智能指针管理
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;
    int m_thread_num;  // CPU线程池大小

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器相关
    std::unique_ptr<client_data[]> users_timer;  // 智能指针管理数组
    Utils utils;

    // 依赖注入的管理器（使用智能指针）
    std::unique_ptr<UserManager> m_user_manager;    // 用户管理器
    std::unique_ptr<StaticCache> m_static_cache;    // 静态文件缓存
    std::unique_ptr<Log> m_logger;                  // 日志系统
    std::unique_ptr<IoUringManager> m_io_uring_manager;  // io_uring 管理器

#ifdef USE_COROUTINE
    // 协程+线程池混合架构组件
    std::unique_ptr<AdvancedScheduler> m_coro_scheduler;    // 增强型协程调度器
    std::unique_ptr<CpuThreadPool> m_cpu_thread_pool;       // CPU任务线程池
    std::unique_ptr<TaskDispatcher> m_task_dispatcher;      // 智能任务分发器
#endif
};

#endif
