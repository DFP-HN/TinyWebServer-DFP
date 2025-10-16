#include "epoll_manager.h"
#include <errno.h>

EpollManager::EpollManager() : m_epollfd(-1)
{
}

EpollManager::~EpollManager()
{
    if (m_epollfd != -1)
    {
        close(m_epollfd);
        m_epollfd = -1;
    }
}

bool EpollManager::create(int size)
{
    m_epollfd = epoll_create(size);
    return m_epollfd != -1;
}

int EpollManager::wait(epoll_event *events, int maxevents, int timeout)
{
    return epoll_wait(m_epollfd, events, maxevents, timeout);
}

int EpollManager::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

bool EpollManager::addfd(int fd, bool one_shot, int trig_mode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == trig_mode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;

    int ret = epoll_ctl(m_epollfd, EPOLL_CTL_ADD, fd, &event);
    if (ret == 0)
    {
        setnonblocking(fd);
        return true;
    }
    return false;
}

bool EpollManager::removefd(int fd)
{
    int ret = epoll_ctl(m_epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
    return ret == 0;
}

bool EpollManager::modfd(int fd, int ev, int trig_mode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == trig_mode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    int ret = epoll_ctl(m_epollfd, EPOLL_CTL_MOD, fd, &event);
    return ret == 0;
}
