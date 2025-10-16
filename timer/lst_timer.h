#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;
class EpollManager;
class UserManager;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

// 定时器回调函数类型
// 使用依赖注入的方式，移除全局静态变量访问
typedef void (*timer_callback)(client_data *user_data, EpollManager *epoll_mgr, UserManager *user_mgr);

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;

    timer_callback cb_func;
    client_data *user_data;
    util_timer *prev;
    util_timer *next;

    // 保存依赖注入的指针
    EpollManager *epoll_manager;
    UserManager *user_manager;
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

// 重构后的Utils类 - 移除静态成员
class Utils
{
public:
    Utils();
    ~Utils();

    void init(int timeslot);

    // 依赖注入
    void set_epoll_manager(EpollManager *epoll_mgr);
    void set_signal_pipe(int *pipefd);

    // 信号处理函数 - 改为非静态
    void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    sort_timer_lst m_timer_lst;
    int m_TIMESLOT;

private:
    int *m_pipefd;              // 依赖注入的管道fd
    EpollManager *m_epoll_manager; // 依赖注入的epoll管理器
};

// 重构后的回调函数
void cb_func(client_data *user_data, EpollManager *epoll_mgr, UserManager *user_mgr);

// 设置全局Utils实例（用于信号处理）
void set_global_utils_instance(Utils *utils);

#endif
