#include "lst_timer.h"
#include "../user/user_manager.h"

sort_timer_lst::sort_timer_lst()
{
    head = nullptr;
    tail = nullptr;
}

sort_timer_lst::~sort_timer_lst()
{
    // 智能指针自动释放，只需断开链表
    while (head)
    {
        auto tmp = head;
        head = head->next;
        if (head)
        {
            head->prev = nullptr;  // 断开反向引用
        }
        tmp->next = nullptr;  // 断开正向引用，释放智能指针
    }
    tail = nullptr;
}

void sort_timer_lst::add_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }
    auto tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        if (head)
        {
            head->prev = nullptr;
        }
        timer->next = nullptr;
        add_timer(timer, head);
    }
    else
    {
        auto prev_timer = timer->prev;
        auto next_timer = timer->next;
        if (prev_timer)
        {
            prev_timer->next = next_timer;
        }
        if (next_timer)
        {
            next_timer->prev = prev_timer;
        }
        timer->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, next_timer);
    }
}

void sort_timer_lst::del_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        head = nullptr;
        tail = nullptr;
        return;  // 智能指针自动释放
    }
    if (timer == head)
    {
        head = head->next;
        if (head)
        {
            head->prev = nullptr;
        }
        return;  // 智能指针自动释放
    }
    if (timer == tail)
    {
        tail = tail->prev;
        if (tail)
        {
            tail->next = nullptr;
        }
        return;  // 智能指针自动释放
    }
    auto prev_timer = timer->prev;
    auto next_timer = timer->next;
    if (prev_timer)
    {
        prev_timer->next = next_timer;
    }
    if (next_timer)
    {
        next_timer->prev = prev_timer;
    }
    timer->prev = nullptr;
    timer->next = nullptr;
    // 智能指针自动释放
}

void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL);
    auto tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        // 调用回调函数，传递依赖注入的对象
        tmp->cb_func(tmp->user_data, tmp->user_manager);
        head = tmp->next;
        if (head)
        {
            head->prev = nullptr;
        }
        tmp->next = nullptr;  // 断开引用
        tmp = head;  // 智能指针自动释放旧的 tmp
    }
}

void sort_timer_lst::add_timer(std::shared_ptr<util_timer> timer, std::shared_ptr<util_timer> lst_head)
{
    auto prev = lst_head;
    auto tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}

// ========================================
// 最小堆定时器实现（高性能 O(log n)）
// ========================================

heap_timer_lst::heap_timer_lst()
{
    m_heap.reserve(1024);  // 预分配空间，减少扩容
}

heap_timer_lst::~heap_timer_lst()
{
    m_heap.clear();
    m_timer_pos.clear();
}

void heap_timer_lst::add_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }

    // 添加到堆尾
    m_heap.push_back(timer);
    size_t pos = m_heap.size() - 1;
    m_timer_pos[timer.get()] = pos;

    // 上浮到正确位置 O(log n)
    sift_up(pos);
}

void heap_timer_lst::adjust_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }

    // 查找定时器位置 O(1)
    auto it = m_timer_pos.find(timer.get());
    if (it == m_timer_pos.end())
    {
        return;  // 定时器不在堆中
    }

    size_t pos = it->second;

    // 由于expire已被更新，只需重新堆化
    // 尝试上浮
    sift_up(pos);
    // 如果上浮没动，则可能需要下沉
    if (it->second == pos)  // 位置未变，尝试下沉
    {
        sift_down(pos);
    }
}

void heap_timer_lst::del_timer(std::shared_ptr<util_timer> timer)
{
    if (!timer)
    {
        return;
    }

    // 查找定时器位置
    auto it = m_timer_pos.find(timer.get());
    if (it == m_timer_pos.end())
    {
        return;  // 定时器不在堆中
    }

    size_t pos = it->second;

    // 删除：用堆尾元素替换，然后重新堆化
    m_timer_pos.erase(timer.get());

    if (pos == m_heap.size() - 1)
    {
        // 删除的是堆尾元素，直接弹出
        m_heap.pop_back();
    }
    else
    {
        // 用堆尾替换被删除元素
        m_heap[pos] = m_heap.back();
        m_heap.pop_back();

        if (!m_heap.empty() && pos < m_heap.size())
        {
            m_timer_pos[m_heap[pos].get()] = pos;
            // 重新堆化
            sift_up(pos);
            sift_down(pos);
        }
    }
}

void heap_timer_lst::tick()
{
    time_t cur = time(NULL);

    // 处理所有过期的定时器
    while (!m_heap.empty())
    {
        auto timer = m_heap[0];

        // 检查是否过期
        if (cur < timer->expire)
        {
            break;  // 堆顶未过期，后面的都不会过期
        }

        // 调用回调函数
        if (timer->cb_func)
        {
            timer->cb_func(timer->user_data, timer->user_manager);
        }

        // 移除堆顶 O(log n)
        m_timer_pos.erase(timer.get());

        if (m_heap.size() > 1)
        {
            m_heap[0] = m_heap.back();
            m_heap.pop_back();
            if (!m_heap.empty())
            {
                m_timer_pos[m_heap[0].get()] = 0;
                sift_down(0);
            }
        }
        else
        {
            m_heap.pop_back();
        }
    }
}

// 上浮操作 O(log n)
void heap_timer_lst::sift_up(size_t pos)
{
    while (pos > 0)
    {
        size_t p = parent(pos);
        if (m_heap[pos]->expire >= m_heap[p]->expire)
        {
            break;  // 满足堆性质
        }
        swap_timers(pos, p);
        pos = p;
    }
}

// 下沉操作 O(log n)
void heap_timer_lst::sift_down(size_t pos)
{
    size_t size = m_heap.size();

    while (true)
    {
        size_t smallest = pos;
        size_t l = left_child(pos);
        size_t r = right_child(pos);

        // 找到当前节点、左子节点、右子节点中最小的
        if (l < size && m_heap[l]->expire < m_heap[smallest]->expire)
        {
            smallest = l;
        }
        if (r < size && m_heap[r]->expire < m_heap[smallest]->expire)
        {
            smallest = r;
        }

        if (smallest == pos)
        {
            break;  // 满足堆性质
        }

        swap_timers(pos, smallest);
        pos = smallest;
    }
}

// 交换两个定时器并更新索引
void heap_timer_lst::swap_timers(size_t i, size_t j)
{
    std::swap(m_heap[i], m_heap[j]);
    m_timer_pos[m_heap[i].get()] = i;
    m_timer_pos[m_heap[j].get()] = j;
}

// ========================================

Utils::Utils() : m_TIMESLOT(0), m_pipefd(NULL)
{
}

Utils::~Utils()
{
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

void Utils::set_signal_pipe(int *pipefd)
{
    m_pipefd = pipefd;
}

// 信号处理函数 - 现在是非静态的，但需要通过全局函数桥接
// 因为信号处理函数必须是普通函数指针
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    if (m_pipefd)
    {
        send(m_pipefd[1], (char *)&msg, 1, 0);
    }
    errno = save_errno;
}

// 全局Utils实例指针，用于信号处理函数桥接
static Utils *g_utils_instance = NULL;

// 全局信号处理函数，桥接到Utils实例
// 改为非静态，以便在其他文件中使用
void global_sig_handler(int sig)
{
    if (g_utils_instance)
    {
        g_utils_instance->sig_handler(sig);
    }
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 重构后的回调函数 - 移除EpollManager依赖
void cb_func(client_data *user_data, UserManager *user_mgr)
{
    assert(user_data);

    // 直接关闭socket（不再使用epoll）
    close(user_data->sockfd);

    if (user_mgr)
    {
        user_mgr->decrement_user_count();
    }
}

// 设置全局Utils实例（用于信号处理）
void set_global_utils_instance(Utils *utils)
{
    g_utils_instance = utils;
}
