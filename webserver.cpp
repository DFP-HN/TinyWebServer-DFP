#include "webserver.h"
#include <cstdlib>  // for getenv

WebServer::WebServer()
{
    // 创建管理器实例（使用 make_unique 智能指针）
    m_epoll_manager = std::make_unique<EpollManager>();
    m_user_manager = std::make_unique<UserManager>();
    m_static_cache = std::make_unique<StaticCache>(256);  // 256MB缓存

// #ifdef USE_IO_URING
    // 创建 io_uring 管理器（队列深度 256）
    m_io_uring_manager = std::make_unique<IoUringManager>(256);
// #endif

    // 创建 http_conn 对象数组（使用智能指针）
    users = std::make_unique<http_conn[]>(MAX_FD);

    // 为所有 http_conn 对象注入依赖
    for (int i = 0; i < MAX_FD; ++i)
    {
        users[i].set_epoll_manager(m_epoll_manager.get());
        users[i].set_user_manager(m_user_manager.get());
        users[i].set_static_cache(m_static_cache.get());
    }

    // root文件夹路径（使用智能指针）
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    size_t root_len = strlen(server_path) + strlen(root) + 1;
    m_root = std::make_unique<char[]>(root_len);
    strcpy(m_root.get(), server_path);
    strcat(m_root.get(), root);

    // 定时器（使用智能指针）
    users_timer = std::make_unique<client_data[]>(MAX_FD);
}

WebServer::~WebServer()
{
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    // 智能指针自动释放，无需手动 delete
    // users、users_timer、m_pool、m_epoll_manager、m_user_manager、m_root
    // 都会自动调用析构函数
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model, int event_loop_mode)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
    m_event_loop_mode = event_loop_mode;

#ifdef USE_IO_URING
    // 如果使用 io_uring 模式，初始化 io_uring 管理器
    if (m_event_loop_mode == IO_URING_MODE && m_io_uring_manager)
    {
        if (!m_io_uring_manager->init())
        {
            LOG_ERROR("Failed to initialize io_uring, falling back to epoll mode");
            m_event_loop_mode = EPOLL_MODE;
        }
        else
        {
            LOG_INFO("io_uring initialized successfully");
        }
    }
#else
    // 如果没有编译 io_uring 支持，强制使用 epoll 模式
    if (m_event_loop_mode == IO_URING_MODE)
    {
        LOG_WARN("io_uring not compiled, falling back to epoll mode");
        m_event_loop_mode = EPOLL_MODE;
    }
#endif
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 创建日志实例（使用智能指针）
        m_logger = std::make_unique<Log>();

        //初始化日志
        if (1 == m_log_write)
            m_logger->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            m_logger->init("./ServerLog", m_close_log, 2000, 800000, 0);

        // 设置全局日志实例（用于向后兼容的宏）
        g_log_instance = m_logger.get();
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();

    // 从环境变量获取数据库主机名，默认为 localhost
    const char *db_host = getenv("DB_HOST");
    if (db_host == NULL) {
        db_host = "localhost";
    }

    m_connPool->init(db_host, m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表（只需要在第一个http_conn对象上调用一次）
    users[0].initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池（使用智能指针）
    m_pool = std::make_unique<threadpool<http_conn>>(m_actormodel, m_connPool, m_thread_num);
    // m_pool = std::make_unique<WorkStealingPool<http_conn>>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 初始化 Utils
    utils.init(TIMESLOT);

    // 创建 epoll 实例
    m_epoll_manager->create(5);

    // 注册监听 socket
    m_epoll_manager->addfd(m_listenfd, false, m_LISTENTrigmode);

    // 创建信号管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    EpollManager::setnonblocking(m_pipefd[1]);
    m_epoll_manager->addfd(m_pipefd[0], false, 0);

    // 设置 Utils 的依赖（智能指针用 .get() 获取原始指针）
    utils.set_epoll_manager(m_epoll_manager.get());
    utils.set_signal_pipe(m_pipefd);

    // 设置全局 Utils 实例（用于信号处理）
    set_global_utils_instance(&utils);

    // 注册信号
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, global_sig_handler, false);
    utils.addsig(SIGTERM, global_sig_handler, false);

    alarm(TIMESLOT);
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root.get(), m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    // 使用智能指针创建定时器
    auto timer = std::make_shared<util_timer>();
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;

    // 设置定时器的依赖注入（智能指针用 .get() 获取原始指针）
    timer->epoll_manager = m_epoll_manager.get();
    timer->user_manager = m_user_manager.get();

    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;  // shared_ptr 赋值
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(std::shared_ptr<util_timer> timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(std::shared_ptr<util_timer> timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd], m_epoll_manager.get(), m_user_manager.get());
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (m_user_manager->get_user_count() >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (m_user_manager->get_user_count() >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    auto timer = users_timer[sockfd].timer;  // shared_ptr

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(&users[sockfd], 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(&users[sockfd]);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    auto timer = users_timer[sockfd].timer;  // shared_ptr
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(&users[sockfd], 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = m_epoll_manager->wait(events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                auto timer = users_timer[sockfd].timer;  // shared_ptr
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}

#ifdef USE_IO_URING
// io_uring user_data 编码：高32位=操作类型，低32位=fd
// 操作类型：0=accept, 1=read, 2=write
#define OP_ACCEPT 0ULL
#define OP_READ   1ULL
#define OP_WRITE  2ULL
#define ENCODE_USER_DATA(op, fd) (((uint64_t)(op) << 32) | (uint64_t)(fd))
#define DECODE_OP(user_data) ((user_data) >> 32)
#define DECODE_FD(user_data) ((int)((user_data) & 0xFFFFFFFF))

// io_uring 事件循环
void WebServer::eventLoop_uring()
{
    if (!m_io_uring_manager || !m_io_uring_manager->is_initialized())
    {
        LOG_ERROR("io_uring not initialized, cannot start event loop");
        return;
    }

    LOG_INFO("Starting io_uring event loop (queue depth=%u)", m_io_uring_manager->get_queue_depth());

    bool timeout = false;
    bool stop_server = false;

    // 为 accept 分配独立的地址结构（避免被覆盖）
    struct sockaddr_in *accept_addr = new struct sockaddr_in;
    socklen_t *accept_addrlen = new socklen_t;
    *accept_addrlen = sizeof(struct sockaddr_in);

    // 提交监听 socket 的 accept 请求（异步接受连接）
    m_io_uring_manager->submit_accept(m_listenfd, (struct sockaddr*)accept_addr,
                                       accept_addrlen, ENCODE_USER_DATA(OP_ACCEPT, m_listenfd));

    while (!stop_server)
    {
        // 批量提交所有待处理的 SQ 条目
        int submitted = m_io_uring_manager->submit_all();
        if (submitted < 0)
        {
            LOG_ERROR("io_uring_submit failed");
            break;
        }

        // 等待至少一个完成事件
        int ready = m_io_uring_manager->wait_completions(1);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                // 被信号中断，检查定时器
                if (timeout)
                {
                    utils.timer_handler();
                    LOG_INFO("%s", "timer tick");
                    timeout = false;
                }
                continue;
            }
            LOG_ERROR("io_uring_wait_completions failed");
            break;
        }

        // 处理所有完成事件
        int processed = m_io_uring_manager->process_completions([this, accept_addr, accept_addrlen]
                                                                 (struct io_uring_cqe *cqe) {
            handle_io_completion(cqe);

            // 如果是 accept 完成且成功，重新提交 accept 请求（持续监听）
            uint64_t user_data = (uint64_t)io_uring_cqe_get_data(cqe);
            uint64_t op_type = DECODE_OP(user_data);

            if (op_type == OP_ACCEPT && cqe->res > 0)
            {
                // accept 成功，重新提交 accept 请求
                *accept_addrlen = sizeof(struct sockaddr_in);
                m_io_uring_manager->submit_accept(m_listenfd, (struct sockaddr*)accept_addr,
                                                   accept_addrlen, ENCODE_USER_DATA(OP_ACCEPT, m_listenfd));
            }
        });

        if (processed < 0)
        {
            LOG_ERROR("process_completions failed");
            break;
        }

        // 处理定时器
        if (timeout)
        {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }

    // 清理
    delete accept_addr;
    delete accept_addrlen;

    LOG_INFO("io_uring event loop stopped");
}

// 处理 io_uring 完成事件
void WebServer::handle_io_completion(struct io_uring_cqe *cqe)
{
    if (!cqe)
    {
        return;
    }

    uint64_t user_data = (uint64_t)io_uring_cqe_get_data(cqe);
    int res = cqe->res;

    // 解码操作类型和文件描述符
    uint64_t op_type = DECODE_OP(user_data);
    int sockfd = DECODE_FD(user_data);

    if (op_type == OP_ACCEPT)
    {
        // accept 操作完成
        if (res < 0)
        {
            LOG_ERROR("async accept failed: %s", strerror(-res));
            return;
        }

        int connfd = res;
        LOG_INFO("async accept: new connection fd=%d", connfd);

        // 获取客户端地址
        struct sockaddr_in client_address;
        socklen_t client_addrlength = sizeof(client_address);
        getpeername(connfd, (struct sockaddr*)&client_address, &client_addrlength);

        // 初始化连接和定时器
        timer(connfd, client_address);

        // 提交异步读请求
        m_io_uring_manager->submit_read(connfd, users[connfd].get_read_buffer(),
                                         http_conn::READ_BUFFER_SIZE, -1,
                                         ENCODE_USER_DATA(OP_READ, connfd));
    }
    else if (op_type == OP_READ)
    {
        // 读操作完成
        if (sockfd < 0 || sockfd >= MAX_FD)
        {
            LOG_ERROR("Invalid sockfd=%d from io_uring read completion", sockfd);
            return;
        }

        if (res < 0)
        {
            // I/O 错误，关闭连接
            LOG_DEBUG("async read error on fd=%d: %s", sockfd, strerror(-res));
            auto timer = users_timer[sockfd].timer;
            deal_timer(timer, sockfd);
            return;
        }

        if (res == 0)
        {
            // 连接关闭
            LOG_DEBUG("Connection closed by peer: fd=%d", sockfd);
            auto timer = users_timer[sockfd].timer;
            deal_timer(timer, sockfd);
            return;
        }

        // 读取成功
        http_conn *conn = &users[sockfd];
        conn->get_read_idx() += res;
        LOG_DEBUG("async read completed: fd=%d, bytes=%d, total=%ld",
                  sockfd, res, conn->get_read_idx());

        // 调整定时器
        auto timer = users_timer[sockfd].timer;
        if (timer)
        {
            adjust_timer(timer);
        }

        // io_uring 模式下，在主线程中处理 HTTP（避免线程池开销）
        // 因为 io_uring 本身就是异步的，不需要再用线程池
        conn->process();

        // 检查是否有响应需要发送
        if (conn->get_bytes_to_send() > 0)
        {
            // 提交异步写请求
            struct iovec *iv = conn->get_iovec();
            int iv_count = conn->get_iovec_count();

            // 先发送响应头（第一块数据）
            if (iv_count > 0 && iv[0].iov_len > 0)
            {
                m_io_uring_manager->submit_write(sockfd, iv[0].iov_base, iv[0].iov_len, -1,
                                                  ENCODE_USER_DATA(OP_WRITE, sockfd));
            }
            // 注意：文件内容会在写完成后发送
        }
        else
        {
            // 没有数据要发送，继续读取
            conn->reset_connection();
            m_io_uring_manager->submit_read(sockfd, conn->get_read_buffer(),
                                             http_conn::READ_BUFFER_SIZE, -1,
                                             ENCODE_USER_DATA(OP_READ, sockfd));
        }
    }
    else if (op_type == OP_WRITE)
    {
        // 写操作完成
        if (sockfd < 0 || sockfd >= MAX_FD)
        {
            LOG_ERROR("Invalid sockfd=%d from io_uring write completion", sockfd);
            return;
        }

        if (res < 0)
        {
            // I/O 错误，关闭连接
            LOG_DEBUG("async write error on fd=%d: %s", sockfd, strerror(-res));
            auto timer = users_timer[sockfd].timer;
            deal_timer(timer, sockfd);
            return;
        }

        // 写入成功
        http_conn *conn = &users[sockfd];
        LOG_DEBUG("async write completed: fd=%d, bytes=%d", sockfd, res);

        // 更新已发送的字节数
        conn->add_bytes_have_send(res);

        // 获取当前状态
        int bytes_sent = conn->get_bytes_have_send();
        int bytes_total = conn->get_bytes_to_send();
        int write_idx = conn->get_write_idx();

        LOG_DEBUG("Write state: fd=%d, sent=%d, total=%d, write_idx=%d, using_sendfile=%d",
                  sockfd, bytes_sent, bytes_total, write_idx, conn->is_using_sendfile());

        // 检查是否还有数据要发送
        if (bytes_sent < bytes_total)
        {
            // 判断响应头是否已经发送完成
            if (bytes_sent >= write_idx)
            {
                // 响应头已发送完成，现在需要发送文件内容
                if (conn->is_using_sendfile())
                {
                    // io_uring 的 splice 操作可能不支持文件->socket直接传输
                    // 改用 read + write 方案：分配缓冲区，读取文件内容，然后发送

                    int file_fd = conn->get_file_fd();
                    off_t file_offset = bytes_sent - write_idx;
                    size_t file_size = conn->get_file_size();
                    size_t file_remaining = file_size - file_offset;

                    LOG_DEBUG("Sending file content: fd=%d, file_fd=%d, offset=%ld, remaining=%zu",
                              sockfd, file_fd, file_offset, file_remaining);

                    // 使用 pread 读取文件内容到缓冲区，然后用 io_uring write
                    // 为了避免内存分配，我们一次最多发送 128KB
                    const size_t CHUNK_SIZE = 128 * 1024;
                    size_t send_size = (file_remaining > CHUNK_SIZE) ? CHUNK_SIZE : file_remaining;

                    // 分配临时缓冲区（存储在 http_conn 中，在下次写完成或连接关闭时释放）
                    conn->clear_file_buffer();  // 清理旧缓冲区
                    char *file_buffer = new char[send_size];
                    conn->set_file_buffer(file_buffer);

                    ssize_t read_bytes = pread(file_fd, file_buffer, send_size, file_offset);

                    if (read_bytes > 0)
                    {
                        m_io_uring_manager->submit_write(sockfd, file_buffer, read_bytes, -1,
                                                          ENCODE_USER_DATA(OP_WRITE, sockfd));
                    }
                    else
                    {
                        LOG_ERROR("pread failed for fd=%d: %s", file_fd, strerror(errno));
                        conn->clear_file_buffer();
                        auto timer = users_timer[sockfd].timer;
                        deal_timer(timer, sockfd);
                    }
                }
                else
                {
                    // 使用 mmap 方式，发送第二块数据（文件内容）
                    struct iovec *iv = conn->get_iovec();
                    int iv_count = conn->get_iovec_count();

                    if (iv_count > 1 && iv[1].iov_len > 0)
                    {
                        // 计算已经发送的文件内容偏移
                        size_t file_sent = bytes_sent - write_idx;
                        char *file_base = (char*)iv[1].iov_base + file_sent;
                        size_t file_remaining = iv[1].iov_len - file_sent;

                        m_io_uring_manager->submit_write(sockfd, file_base, file_remaining, -1,
                                                          ENCODE_USER_DATA(OP_WRITE, sockfd));
                    }
                }
            }
            else
            {
                // 响应头还没发送完，继续发送响应头
                struct iovec *iv = conn->get_iovec();
                char *header_base = (char*)iv[0].iov_base + bytes_sent;
                size_t header_remaining = write_idx - bytes_sent;

                m_io_uring_manager->submit_write(sockfd, header_base, header_remaining, -1,
                                                  ENCODE_USER_DATA(OP_WRITE, sockfd));
            }
        }
        else
        {
            // 所有数据发送完成
            LOG_DEBUG("All data sent: fd=%d, total=%d bytes", sockfd, bytes_total);

            auto timer = users_timer[sockfd].timer;
            if (timer)
            {
                adjust_timer(timer);
            }

            // 检查是否保持连接
            if (conn->get_linger())
            {
                // 保持连接，重置并继续读取
                conn->reset_connection();
                m_io_uring_manager->submit_read(sockfd, conn->get_read_buffer(),
                                                 http_conn::READ_BUFFER_SIZE, -1,
                                                 ENCODE_USER_DATA(OP_READ, sockfd));
            }
            else
            {
                // 关闭连接
                deal_timer(timer, sockfd);
            }
        }
    }
    else
    {
        LOG_ERROR("Unknown operation type: %lu", op_type);
    }
}

#endif // USE_IO_URING

