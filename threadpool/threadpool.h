#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        // 批处理优化：一次性取出多个任务，减少锁竞争
        // 设置批处理大小为 4（可根据实际负载调整）
        const int BATCH_SIZE = 4;
        std::list<T*> local_batch;

        // 第一个任务通过信号量等待
        m_queuestat.wait();

        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        // 批量取出任务到本地队列（最多 BATCH_SIZE 个）
        // 第一个任务已经通过信号量获取，直接取出
        local_batch.push_back(m_workqueue.front());
        m_workqueue.pop_front();

        // 尝试额外取出更多任务（非阻塞）
        int batch_count = 1;
        while (!m_workqueue.empty() && batch_count < BATCH_SIZE)
        {
            // 额外的任务也需要对应的信号量计数
            if (m_queuestat.trywait())  // 尝试非阻塞获取信号量
            {
                local_batch.push_back(m_workqueue.front());
                m_workqueue.pop_front();
                batch_count++;
            }
            else
            {
                break;  // 没有更多信号量，停止批量取出
            }
        }
        m_queuelocker.unlock();

        // 处理本地批次的所有任务（无需持有全局锁）
        for (typename std::list<T*>::iterator it = local_batch.begin();
             it != local_batch.end(); ++it)
        {
            T *request = *it;
            if (!request)
                continue;

            // 处理单个任务
            if (1 == m_actor_model)
            {
                if (0 == request->m_state)
                {
                    if (request->read_once())
                    {
                        request->improv = 1;
                        connectionRAII mysqlcon(&request->mysql, m_connPool);
                        request->process();
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
                else
                {
                    if (request->write())
                    {
                        request->improv = 1;
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
            }
            else
            {
                connectionRAII mysqlcon(&request->mysql, m_connPool);
                request->process();
            }
        } // end for loop
    } // end while true
}
#endif
