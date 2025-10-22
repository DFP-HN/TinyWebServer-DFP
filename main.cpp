#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num,
                config.close_log, config.actor_model, config.event_loop_mode);


    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //根据配置选择事件循环模式
    fprintf(stderr, "[DEBUG] main: event_loop_mode = %d\n", config.event_loop_mode);
    fflush(stderr);

#ifdef USE_COROUTINE
    if (config.event_loop_mode == COROUTINE_MODE)
    {
        fprintf(stderr, "[DEBUG] main: Entering COROUTINE_MODE\n");
        fflush(stderr);
        LOG_INFO("Using coroutine event loop (io_uring + C++20 coroutines)");
        server.eventLoop_coro();
    }
    else
#endif
#ifdef USE_IO_URING
    if (config.event_loop_mode == IO_URING_MODE)
    {
        fprintf(stderr, "[DEBUG] main: Entering IO_URING_MODE\n");
        fflush(stderr);
        LOG_INFO("Using io_uring event loop (callback style)");
        server.eventLoop_uring();
    }
    else
#endif
    {
        fprintf(stderr, "[DEBUG] main: Entering EPOLL_MODE\n");
        fflush(stderr);
        LOG_INFO("Using epoll event loop (traditional)");
        server.eventLoop();
    }

    fprintf(stderr, "[DEBUG] main: Event loop returned\n");
    fflush(stderr);
    return 0;
}
