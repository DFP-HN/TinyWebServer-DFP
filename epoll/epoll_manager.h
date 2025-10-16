#ifndef EPOLL_MANAGER_H
#define EPOLL_MANAGER_H

#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

// Epoll操作管理器 - 封装所有epoll相关操作
// 消除全局函数和静态依赖
class EpollManager
{
public:
    EpollManager();
    ~EpollManager();

    // 创建epoll实例
    bool create(int size = 5);

    // 获取epoll文件描述符
    int get_epollfd() const { return m_epollfd; }

    // 等待事件
    int wait(epoll_event *events, int maxevents, int timeout);

    // 文件描述符设置为非阻塞
    static int setnonblocking(int fd);

    // 注册fd到epoll
    bool addfd(int fd, bool one_shot, int trig_mode);

    // 从epoll删除fd
    bool removefd(int fd);

    // 修改fd的epoll事件
    bool modfd(int fd, int ev, int trig_mode);

private:
    int m_epollfd;
};

#endif
