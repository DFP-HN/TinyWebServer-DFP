/*************************************************************
 * 重构后的阻塞队列 - 使用智能指针和RAII
 *
 * 改进：
 * 1. 使用 std::vector 替代原始指针数组
 * 2. RAII 锁管理，异常安全
 * 3. 抛出异常替代 exit(-1)
 * 4. 保持线程安全和接口兼容
 *************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <vector>
#include <stdexcept>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

// RAII 锁管理辅助类
class LockGuard
{
public:
    explicit LockGuard(locker& lock) : m_lock(lock)
    {
        m_lock.lock();
    }
    ~LockGuard()
    {
        m_lock.unlock();
    }

    // 禁用拷贝
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    locker& m_lock;
};

template <class T>
class block_queue
{
public:
    // 构造函数 - 抛出异常而不是 exit
    explicit block_queue(int max_size = 1000)
        : m_max_size(max_size), m_size(0), m_front(-1), m_back(-1)
    {
        if (max_size <= 0)
        {
            throw std::invalid_argument("block_queue: max_size must be positive");
        }

        m_array.resize(max_size);
    }

    // 禁用拷贝构造和赋值（队列不应被拷贝）
    block_queue(const block_queue&) = delete;
    block_queue& operator=(const block_queue&) = delete;

    // 清空队列
    void clear()
    {
        LockGuard lock(m_mutex);
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    // 析构函数 - RAII 自动管理资源
    ~block_queue()
    {
        clear();
    }

    // 判断队列是否满
    bool full()
    {
        LockGuard lock(m_mutex);
        return m_size >= m_max_size;
    }

    // 判断队列是否为空
    bool empty()
    {
        LockGuard lock(m_mutex);
        return m_size == 0;
    }

    // 返回队首元素
    bool front(T &value)
    {
        LockGuard lock(m_mutex);
        if (m_size == 0)
        {
            return false;
        }
        value = m_array[m_front];
        return true;
    }

    // 返回队尾元素
    bool back(T &value)
    {
        LockGuard lock(m_mutex);
        if (m_size == 0)
        {
            return false;
        }
        value = m_array[m_back];
        return true;
    }

    // 当前队列大小
    int size()
    {
        LockGuard lock(m_mutex);
        return m_size;
    }

    // 最大队列大小
    int max_size()
    {
        LockGuard lock(m_mutex);
        return m_max_size;
    }

    // 添加元素到队列
    // 当队列满时，唤醒所有等待线程并返回false
    bool push(const T &item)
    {
        m_mutex.lock();  // 手动锁定（因为需要在广播前解锁）

        if (m_size >= m_max_size)
        {
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        m_cond.broadcast();  // 唤醒等待的消费者
        m_mutex.unlock();
        return true;
    }

    // 从队列取出元素（阻塞）
    // 如果队列为空，等待条件变量
    bool pop(T &item)
    {
        m_mutex.lock();

        while (m_size <= 0)
        {
            if (!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;

        m_mutex.unlock();
        return true;
    }

    // 从队列取出元素（超时）
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);

        m_mutex.lock();

        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000000;  // 修正：微秒转纳秒

            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;

        m_mutex.unlock();
        return true;
    }

private:
    mutable locker m_mutex;  // mutable 允许在 const 函数中加锁
    cond m_cond;

    std::vector<T> m_array;  // 使用 vector 自动管理内存
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
