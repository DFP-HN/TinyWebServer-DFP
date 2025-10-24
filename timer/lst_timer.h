#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
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
#include <memory>  // 智能指针
#include <vector>
#include <unordered_map>
#include "../log/log.h"

class util_timer;
class UserManager;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    std::shared_ptr<util_timer> timer;  // 使用智能指针管理定时器
};

// 定时器回调函数类型（移除EpollManager依赖）
typedef void (*timer_callback)(client_data *user_data, UserManager *user_mgr);

class util_timer
{
public:
    util_timer() : prev(nullptr), next(nullptr) {}

public:
    time_t expire;

    timer_callback cb_func;
    client_data *user_data;
    std::shared_ptr<util_timer> prev;  // 使用智能指针
    std::shared_ptr<util_timer> next;  // 使用智能指针

    // 保存依赖注入的指针
    UserManager *user_manager;
};

// 排序链表定时器（旧实现）
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(std::shared_ptr<util_timer> timer);
    void adjust_timer(std::shared_ptr<util_timer> timer);
    void del_timer(std::shared_ptr<util_timer> timer);
    void tick();

private:
    void add_timer(std::shared_ptr<util_timer> timer, std::shared_ptr<util_timer> lst_head);

    std::shared_ptr<util_timer> head;  // 使用智能指针
    std::shared_ptr<util_timer> tail;  // 使用智能指针
};

// 最小堆定时器（新实现 - 高性能）
class heap_timer_lst
{
public:
    heap_timer_lst();
    ~heap_timer_lst();

    // 兼容接口
    void add_timer(std::shared_ptr<util_timer> timer);
    void adjust_timer(std::shared_ptr<util_timer> timer);
    void del_timer(std::shared_ptr<util_timer> timer);
    void tick();

    // 堆操作性能：O(log n)
    size_t size() const { return m_heap.size(); }
    bool empty() const { return m_heap.empty(); }

private:
    // 堆操作
    void sift_up(size_t pos);
    void sift_down(size_t pos);
    void swap_timers(size_t i, size_t j);

    // 辅助函数
    size_t parent(size_t i) const { return (i - 1) / 2; }
    size_t left_child(size_t i) const { return 2 * i + 1; }
    size_t right_child(size_t i) const { return 2 * i + 2; }

private:
    std::vector<std::shared_ptr<util_timer>> m_heap;            // 最小堆存储
    std::unordered_map<util_timer*, size_t> m_timer_pos;        // 快速索引
};

// 重构后的Utils类 - 移除epoll依赖
class Utils
{
public:
    Utils();
    ~Utils();

    void init(int timeslot);

    // 依赖注入
    void set_signal_pipe(int *pipefd);

    // 信号处理函数 - 改为非静态
    void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    heap_timer_lst m_timer_lst;  // 使用堆定时器（高性能）
    int m_TIMESLOT;

private:
    int *m_pipefd;              // 依赖注入的管道fd
};

// 重构后的回调函数（移除EpollManager依赖）
void cb_func(client_data *user_data, UserManager *user_mgr);

// 设置全局Utils实例（用于信号处理）
void set_global_utils_instance(Utils *utils);

// 全局信号处理函数（用于信号注册）
void global_sig_handler(int sig);

#endif
