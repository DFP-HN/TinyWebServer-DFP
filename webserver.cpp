#include "webserver.h"
#include <cstdlib>  // for getenv

#ifdef USE_COROUTINE
#include "coroutine/task.h"
#include "coroutine/io_awaiter.h"
#include "coroutine/scheduler.h"
#include "coroutine/advanced_scheduler.h"
#include "coroutine/thread_pool_bridge.h"
#include "coroutine/cpu_task.h"
#include "coroutine/advanced_file_upload.h"
#include "coroutine/advanced_file_download.h"
#include "coroutine/advanced_file_delete.h"
#include "coroutine/file_list_api.h"
#include "coroutine/file_search_api.h"
#include "threadpool/cpu_thread_pool.h"
#include "scheduler/task_dispatcher.h"
#include "monitor/coro_monitor.h"
#include "http/resumable_upload.h"
#include "http/http_range.h"
#endif

WebServer::WebServer()
{
    // 创建管理器实例（使用 make_unique 智能指针）
    m_epoll_manager = std::make_unique<EpollManager>();
    m_user_manager = std::make_unique<UserManager>();
    m_static_cache = std::make_unique<StaticCache>(256);  // 256MB缓存

#ifdef USE_IO_URING
    // 创建 io_uring 管理器（队列深度 256）
    m_io_uring_manager = std::make_unique<IoUringManager>(256);
#endif

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
    // 如果使用 io_uring 模式（回调或协程），初始化 io_uring 管理器
    if ((m_event_loop_mode == IO_URING_MODE || m_event_loop_mode == COROUTINE_MODE) && m_io_uring_manager)
    {
        if (!m_io_uring_manager->init())
        {
            LOG_ERROR("Failed to initialize io_uring, falling back to epoll mode");
            m_event_loop_mode = EPOLL_MODE;
        }
        else
        {
            if (m_event_loop_mode == IO_URING_MODE)
            {
                LOG_INFO("io_uring initialized successfully (callback style)");
            }
            else if (m_event_loop_mode == COROUTINE_MODE)
            {
#ifdef USE_COROUTINE
                LOG_INFO("io_uring initialized successfully (coroutine style)");

                // 创建CPU线程池（线程数=CPU核心数的一半，避免过度竞争）
                size_t cpu_threads = std::thread::hardware_concurrency() / 2;
                if (cpu_threads < 2) cpu_threads = 2;
                if (cpu_threads > 8) cpu_threads = 8;  // 最多8个线程

                m_cpu_thread_pool = std::make_unique<CpuThreadPool>(cpu_threads, 1000);
                LOG_INFO("CPU thread pool created: %zu threads", cpu_threads);

                // 创建任务分发器
                m_task_dispatcher = std::make_unique<TaskDispatcher>();
                LOG_INFO("Task dispatcher created");

                // 创建增强型协程调度器（最多10000个并发协程）
                m_coro_scheduler = std::make_unique<AdvancedScheduler>(
                    m_io_uring_manager.get(), 10000);
                LOG_INFO("Advanced coroutine scheduler created");
#else
                LOG_WARN("Coroutine not compiled, falling back to io_uring callback mode");
                m_event_loop_mode = IO_URING_MODE;
#endif
            }
        }
    }
#else
    // 如果没有编译 io_uring 支持，强制使用 epoll 模式
    if (m_event_loop_mode == IO_URING_MODE || m_event_loop_mode == COROUTINE_MODE)
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

// ==================== 协程实现 ====================

#ifdef USE_COROUTINE
/**
 * @brief 协程版本：处理单个 HTTP 连接的完整生命周期
 *
 * 这个协程负责：
 * 1. 读取客户端请求
 * 2. 解析 HTTP 请求
 * 3. 处理业务逻辑
 * 4. 发送响应
 * 5. 处理 Keep-Alive 或关闭连接
 *
 * @param connfd 客户端连接的文件描述符
 * @param client_address 客户端地址信息
 */
Task<void> WebServer::handle_http_connection_coro(int connfd, struct sockaddr_in client_address)
{
    LOG_INFO("Starting coroutine for connection fd=%d, client=%s",
             connfd, inet_ntoa(client_address.sin_addr));

    // 1. 初始化连接和定时器
    timer(connfd, client_address);
    http_conn* conn = &users[connfd];
    auto timer_obj = users_timer[connfd].timer;

    try {
        // 主循环：处理多个请求（Keep-Alive）
        while (true) {
            // 2. 异步读取客户端请求
            LOG_DEBUG("Coroutine: waiting for read on fd=%d", connfd);

            ssize_t bytes_read = co_await async_read(
                m_io_uring_manager.get(),
                connfd,
                conn->get_read_buffer() + conn->get_read_idx(),
                http_conn::READ_BUFFER_SIZE - conn->get_read_idx(),
                -1  // offset: -1 表示当前位置
            );

            // 检查连接是否关闭
            if (bytes_read == 0) {
                LOG_INFO("Client closed connection: fd=%d", connfd);
                break;
            }

            LOG_DEBUG("Coroutine: read %ld bytes from fd=%d", bytes_read, connfd);

            // 更新读取的字节数
            conn->get_read_idx() += bytes_read;

            // 调整定时器（延长超时时间）
            if (timer_obj) {
                adjust_timer(timer_obj);
            }

            // 3. 解析HTTP请求
            LOG_DEBUG("Coroutine: processing HTTP request for fd=%d", connfd);

            const char* read_buf = conn->get_read_buffer_const();

            // 3.0 检查是否是CPU密集任务请求（优先级最高）
            if (m_task_dispatcher && strstr(read_buf, "/cpu_compute") != nullptr) {
                LOG_INFO("CPU compute request detected for fd=%d", connfd);

                // 解析CPU任务参数
                char task_name[32] = "mixed";  // 默认任务类型
                int level = 3;  // 默认级别

                if (parse_cpu_task_params(read_buf, task_name, sizeof(task_name), &level)) {
                    LOG_INFO("CPU task: type=%s, level=%d", task_name, level);

                    try {
                        // 在线程池执行CPU计算（不阻塞协程）
                        std::string result = co_await dispatch_cpu_task(
                            task_name, level, m_io_uring_manager.get()
                        );

                        // 构造HTTP响应
                        std::string response =
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(result.size()) + "\r\n"
                            "Connection: close\r\n"
                            "\r\n" + result;

                        // 异步写入响应
                        co_await async_write(
                            m_io_uring_manager.get(), connfd,
                            response.c_str(), response.size()
                        );

                        LOG_INFO("CPU task completed: %s level=%d", task_name, level);
                    } catch (const std::exception& e) {
                        LOG_ERROR("CPU task failed: %s", e.what());

                        // 发送错误响应
                        const char* error_response =
                            "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: 20\r\n"
                            "\r\n"
                            "CPU task failed\r\n";
                        co_await async_write(m_io_uring_manager.get(), connfd,
                                             error_response, strlen(error_response));
                    }
                } else {
                    // 参数解析失败
                    const char* error_response =
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 30\r\n"
                        "\r\n"
                        "Invalid CPU task parameters\n";
                    co_await async_write(m_io_uring_manager.get(), connfd,
                                         error_response, strlen(error_response));
                }

                break;  // 关闭连接
            }

            // 3.1 先检查是否可能是文件上传请求（通过简单的缓冲区扫描）
            bool is_upload_request = false;
            std::string content_type_saved;  // 保存Content-Type

            // 快速检查：是否是 POST /upload
            if (strstr(read_buf, "POST /upload HTTP") != nullptr &&
                strstr(read_buf, "multipart/form-data") != nullptr) {
                is_upload_request = true;

                // 在调用process_read()之前提取Content-Type
                const char* ct_ptr = strstr(read_buf, "Content-Type:");
                const char* header_end = strstr(read_buf, "\r\n\r\n");

                if (ct_ptr && header_end && ct_ptr < header_end) {
                    ct_ptr += 13;  // strlen("Content-Type:")
                    while (*ct_ptr == ' ') ct_ptr++;

                    const char* line_end = strstr(ct_ptr, "\r\n");
                    if (line_end) {
                        content_type_saved = std::string(ct_ptr, line_end - ct_ptr);
                        LOG_DEBUG("Extracted Content-Type before parse: %s", content_type_saved.c_str());
                    }
                }
            }

            if (is_upload_request) {
                // 直接从缓冲区提取 Content-Length，不调用 process_read()
                // 因为 process_read() 是为传统 epoll 模式设计的，会破坏协程模式的状态
                size_t content_length = 0;
                const char* cl_ptr = strstr(read_buf, "Content-Length:");
                const char* header_end = strstr(read_buf, "\r\n\r\n");

                if (cl_ptr && header_end && cl_ptr < header_end) {
                    cl_ptr += 15;  // strlen("Content-Length:")
                    while (*cl_ptr == ' ' || *cl_ptr == '\t') cl_ptr++;
                    content_length = atoll(cl_ptr);

                    if (content_length == 0) {
                        LOG_ERROR("Invalid Content-Length value for fd=%d", connfd);
                        break;
                    }

                    fprintf(stderr, "[DEBUG] Before LOG_INFO 1, content_length=%zu\n", content_length);
                    fflush(stderr);
                    LOG_INFO("Detected file upload: Content-Length=%zu", content_length);
                    fprintf(stderr, "[DEBUG] After LOG_INFO 1, before LOG_INFO 2\n");
                    fflush(stderr);
                    LOG_INFO("Content-Type: %s", content_type_saved.c_str());
                    fprintf(stderr, "[DEBUG] After LOG_INFO 2\n");
                    fflush(stderr);
                } else {
                    LOG_ERROR("Invalid upload request: missing Content-Length for fd=%d", connfd);
                    break;
                }

                fprintf(stderr, "[DEBUG] Entering upload logic block\n");
                fflush(stderr);

                if (!content_type_saved.empty() && content_length > 0) {
                    fprintf(stderr, "[DEBUG] Calculating header/body sizes\n");
                    fflush(stderr);

                    // 计算请求头的长度
                    size_t header_length = (header_end + 4) - read_buf;
                    size_t total_read = conn->get_read_idx();

                    fprintf(stderr, "[DEBUG] header_length=%zu, total_read=%zu\n", header_length, total_read);
                    fflush(stderr);

                    LOG_INFO("Header length: %zu, Total read: %zu", header_length, total_read);

                    // 计算已读取的请求体部分（防止整数下溢）
                    size_t body_already_read = 0;
                    if (total_read > header_length) {
                        body_already_read = total_read - header_length;
                    } else {
                        LOG_WARN("total_read (%zu) <= header_length (%zu), setting body_already_read=0",
                                 total_read, header_length);
                    }

                    LOG_INFO("Starting file upload: header=%zu bytes, body_read=%zu/%zu bytes",
                             header_length, body_already_read, content_length);

                    // 配置上传参数
                    AdvancedUploadConfig upload_config;
                    upload_config.enable_md5 = true;
                    upload_config.enable_sha256 = false;
                    upload_config.enable_resume = false;  // 暂时禁用续传
                    upload_config.max_file_size = 2ULL * 1024 * 1024 * 1024; // 2GB
                    upload_config.chunk_size = 128 * 1024; // 128KB

                    // 进度回调
                    auto progress_cb = [connfd](const UploadProgress& progress) {
                        static int last_percent = -1;
                        int current_percent = (int)progress.percentage;
                        if (current_percent % 10 == 0 && current_percent != last_percent) {
                            LOG_INFO("[fd=%d] Upload progress: %.1f%% (%zu/%zu bytes, %.2f MB/s)",
                                     connfd, progress.percentage,
                                     progress.bytes_uploaded, progress.total_bytes,
                                     progress.upload_rate / (1024.0 * 1024.0));
                            last_percent = current_percent;
                        }
                    };

                    // 调用协程文件上传处理器
                    // ⚠️ 重要：必须复制 prebuffer 数据到独立缓冲区！
                    // 因为协程挂起时，conn 的缓冲区可能被修改，导致内存访问错误
                    std::string prebuffer_copy;
                    if (body_already_read > 0) {
                        const char* prebuffer_start = read_buf + header_length;
                        prebuffer_copy.assign(prebuffer_start, body_already_read);
                        fprintf(stderr, "[DEBUG] Copied %zu bytes to prebuffer_copy\n", prebuffer_copy.size());
                        fflush(stderr);
                    }

                    fprintf(stderr, "[DEBUG] Before co_await handle_advanced_file_upload, prebuffer_len=%zu\n", prebuffer_copy.size());
                    fflush(stderr);

                    AdvancedUploadResult upload_result = co_await handle_advanced_file_upload(
                        connfd,
                        m_io_uring_manager.get(),
                        content_type_saved.c_str(),
                        content_length,
                        m_connPool,             // 数据库连接池
                        upload_config,
                        progress_cb,
                        prebuffer_copy.empty() ? nullptr : prebuffer_copy.data(),  // 使用副本
                        prebuffer_copy.size()   // 已读取的字节数
                    );

                    fprintf(stderr, "[DEBUG] After co_await handle_advanced_file_upload, success=%d\n", upload_result.success);
                    fflush(stderr);

                    if (upload_result.success) {
                        LOG_INFO("File upload completed: %s (%zu bytes, %.2fs)",
                                 upload_result.filename.c_str(),
                                 upload_result.bytes_uploaded,
                                 upload_result.upload_time_seconds);
                    } else {
                        LOG_ERROR("File upload failed: %s", upload_result.error_message.c_str());
                    }

                    // 上传处理完成，重置连接
                    conn->reset_connection();

                    // 根据是否Keep-Alive决定是否继续
                    if (!conn->get_linger()) {
                        break;
                    }
                    continue;  // 继续处理下一个请求
                } else {
                    LOG_ERROR("Invalid upload request: empty Content-Type or zero length for fd=%d", connfd);
                    break;
                }
            } else {
                // 3.2 检查API路由和文件操作请求
                fprintf(stderr, "[DEBUG] Checking request type, read_buf first 100 chars: %.100s\n", read_buf);
                fflush(stderr);

                // 3.2.1 检查文件列表API：GET /api/files
                if (strstr(read_buf, "GET /api/files HTTP") != nullptr) {
                    fprintf(stderr, "[DEBUG] File list API request detected!\n");
                    fflush(stderr);

                    // 调用协程文件列表API处理器
                    bool success = co_await handle_file_list_api(
                        connfd,
                        m_io_uring_manager.get(),
                        m_connPool              // 数据库连接池
                    );

                    fprintf(stderr, "[DEBUG] After file list API, success=%d\n", success);
                    fflush(stderr);

                    if (success) {
                        LOG_INFO("File list API request completed for fd=%d", connfd);
                    } else {
                        LOG_ERROR("File list API request failed for fd=%d", connfd);
                    }

                    // API处理完成，重置连接
                    conn->reset_connection();
                    if (!conn->get_linger()) {
                        break;
                    }
                    continue;
                }

                // 3.2.2 检查文件搜索API：GET /api/search?q=...
                if (strstr(read_buf, "GET /api/search") != nullptr) {
                    fprintf(stderr, "[DEBUG] File search API request detected!\n");
                    fflush(stderr);

                    // 解析查询字符串
                    std::string query_string;
                    const char* query_start = strstr(read_buf, "GET /api/search");
                    if (query_start) {
                        query_start = strchr(query_start, '?');
                        if (query_start) {
                            query_start++;  // 跳过 '?'
                            const char* query_end = strstr(query_start, " HTTP/");
                            if (query_end) {
                                query_string.assign(query_start, query_end - query_start);
                            }
                        }
                    }

                    fprintf(stderr, "[DEBUG] Search query string: %s\n", query_string.c_str());
                    fflush(stderr);

                    // 调用协程文件搜索API处理器
                    bool success = co_await handle_file_search_api(
                        connfd,
                        m_io_uring_manager.get(),
                        query_string,
                        m_connPool              // 数据库连接池
                    );

                    fprintf(stderr, "[DEBUG] After file search API, success=%d\n", success);
                    fflush(stderr);

                    if (success) {
                        LOG_INFO("File search API request completed for fd=%d", connfd);
                    } else {
                        LOG_ERROR("File search API request failed for fd=%d", connfd);
                    }

                    // API处理完成，重置连接
                    conn->reset_connection();
                    if (!conn->get_linger()) {
                        break;
                    }
                    continue;
                }

                // 3.2.3 检查文件删除请求：POST /delete/
                if (strstr(read_buf, "POST /delete/") != nullptr) {
                    fprintf(stderr, "[DEBUG] Delete request detected!\n");
                    fflush(stderr);

                    // 手动解析 URL：POST /delete/filename HTTP/1.1
                    const char* url_start = strstr(read_buf, "POST ");
                    if (!url_start) {
                        LOG_ERROR("Invalid POST request for fd=%d", connfd);
                        break;
                    }
                    url_start += 5;  // 跳过 "POST "

                    const char* url_end = strstr(url_start, " HTTP/");
                    if (!url_end) {
                        LOG_ERROR("Invalid HTTP request line for fd=%d", connfd);
                        break;
                    }

                    // 提取 URL
                    char url_buffer[256];
                    size_t url_len = url_end - url_start;
                    if (url_len >= sizeof(url_buffer)) {
                        LOG_ERROR("URL too long for fd=%d", connfd);
                        break;
                    }
                    memcpy(url_buffer, url_start, url_len);
                    url_buffer[url_len] = '\0';

                    fprintf(stderr, "[DEBUG] Extracted delete URL: %s\n", url_buffer);
                    fflush(stderr);

                    // 检查 URL 是否以 /delete/ 开头
                    if (strncmp(url_buffer, "/delete/", 8) != 0) {
                        LOG_ERROR("Invalid delete URL for fd=%d: %s", connfd, url_buffer);
                        break;
                    }

                    // 提取文件名（URL 编码）
                    const char* filename_encoded = url_buffer + 8;  // 跳过 "/delete/"

                    // URL 解码文件名
                    char filename_decoded[256];
                    size_t decoded_len = 0;
                    const char* p = filename_encoded;

                    while (*p && decoded_len < sizeof(filename_decoded) - 1) {
                        if (*p == '%' && p[1] && p[2]) {
                            // 解码 %XX
                            int hex_value = 0;
                            char hex_str[3] = {p[1], p[2], '\0'};
                            if (sscanf(hex_str, "%x", &hex_value) == 1) {
                                filename_decoded[decoded_len++] = (char)hex_value;
                                p += 3;
                            } else {
                                filename_decoded[decoded_len++] = *p++;
                            }
                        } else if (*p == '+') {
                            // '+' 解码为空格
                            filename_decoded[decoded_len++] = ' ';
                            p++;
                        } else {
                            filename_decoded[decoded_len++] = *p++;
                        }
                    }
                    filename_decoded[decoded_len] = '\0';

                    fprintf(stderr, "[DEBUG] Decoded filename for deletion: %s\n", filename_decoded);
                    fflush(stderr);

                    // 调用协程文件删除处理器
                    DeleteResult delete_result = co_await handle_advanced_file_delete(
                        connfd,
                        m_io_uring_manager.get(),
                        filename_decoded,
                        m_connPool              // 数据库连接池
                    );

                    fprintf(stderr, "[DEBUG] After file delete, success=%d\n", delete_result.success);
                    fflush(stderr);

                    if (delete_result.success) {
                        LOG_INFO("File deleted successfully: %s (fd=%d)",
                                 delete_result.filename.c_str(), connfd);
                    } else {
                        LOG_ERROR("File deletion failed: %s (fd=%d, error=%s)",
                                  delete_result.filename.c_str(), connfd,
                                  delete_result.error_message.c_str());
                    }

                    // 删除处理完成，重置连接
                    conn->reset_connection();
                    if (!conn->get_linger()) {
                        break;
                    }
                    continue;
                }

                // 3.2.3 检查文件下载请求：GET /download/
                bool is_download_request = false;
                if (strstr(read_buf, "GET /download/") != nullptr) {
                    is_download_request = true;
                    fprintf(stderr, "[DEBUG] Download request detected!\n");
                    fflush(stderr);
                }

                fprintf(stderr, "[DEBUG] is_download_request = %d\n", is_download_request);
                fflush(stderr);

                if (is_download_request) {
                    // 3.3 文件下载请求（协程处理）
                    // 直接从缓冲区提取 URL，不调用 process_read()（会返回 NO_RESOURCE）
                    fprintf(stderr, "[DEBUG] Extracting URL from buffer for download request\n");
                    fflush(stderr);

                    // 手动解析 URL：GET /download/filename HTTP/1.1
                    const char* url_start = strstr(read_buf, "GET ");
                    if (!url_start) {
                        LOG_ERROR("Invalid GET request for fd=%d", connfd);
                        break;
                    }
                    url_start += 4;  // 跳过 "GET "

                    const char* url_end = strstr(url_start, " HTTP/");
                    if (!url_end) {
                        LOG_ERROR("Invalid HTTP request line for fd=%d", connfd);
                        break;
                    }

                    // 提取 URL
                    char url_buffer[256];
                    size_t url_len = url_end - url_start;
                    if (url_len >= sizeof(url_buffer)) {
                        LOG_ERROR("URL too long for fd=%d", connfd);
                        break;
                    }
                    memcpy(url_buffer, url_start, url_len);
                    url_buffer[url_len] = '\0';

                    fprintf(stderr, "[DEBUG] Extracted URL: %s\n", url_buffer);
                    fflush(stderr);

                    // 检查 URL 是否以 /download/ 开头
                    if (strncmp(url_buffer, "/download/", 10) != 0) {
                        LOG_ERROR("Invalid download URL for fd=%d: %s", connfd, url_buffer);
                        break;
                    }

                    // 提取文件名（URL 编码）
                    const char* filename_encoded = url_buffer + 10;  // 跳过 "/download/"

                    // URL 解码文件名
                    char filename_decoded[256];
                    size_t decoded_len = 0;
                    const char* p = filename_encoded;

                    while (*p && decoded_len < sizeof(filename_decoded) - 1) {
                        if (*p == '%' && p[1] && p[2]) {
                            // 解码 %XX
                            int hex_value = 0;
                            char hex_str[3] = {p[1], p[2], '\0'};
                            if (sscanf(hex_str, "%x", &hex_value) == 1) {
                                filename_decoded[decoded_len++] = (char)hex_value;
                                p += 3;
                            } else {
                                filename_decoded[decoded_len++] = *p++;
                            }
                        } else if (*p == '+') {
                            // '+' 解码为空格
                            filename_decoded[decoded_len++] = ' ';
                            p++;
                        } else {
                            filename_decoded[decoded_len++] = *p++;
                        }
                    }
                    filename_decoded[decoded_len] = '\0';

                    fprintf(stderr, "[DEBUG] Decoded filename: %s\n", filename_decoded);
                    fflush(stderr);

                    // 构建完整文件路径
                    char file_path[512];
                    snprintf(file_path, sizeof(file_path), "./root/uploads/%s", filename_decoded);

                    fprintf(stderr, "[DEBUG] Download file path: %s\n", file_path);
                    fflush(stderr);

                    // 解析 Range 请求头（如果有）
                    HttpRange range;
                    const char* range_header = conn->get_header("Range");

                    if (range_header) {
                        fprintf(stderr, "[DEBUG] Range header found: %s\n", range_header);
                        fflush(stderr);

                        // 获取文件大小以验证 Range
                        struct stat file_stat;
                        if (stat(file_path, &file_stat) == 0) {
                            // 需要复制 range_header 内容到独立字符串，因为以 \r\n 结尾
                            char range_value[256];
                            const char* value_end = strstr(range_header, "\r\n");
                            if (value_end) {
                                size_t value_len = value_end - range_header;
                                if (value_len < sizeof(range_value)) {
                                    memcpy(range_value, range_header, value_len);
                                    range_value[value_len] = '\0';

                                    if (!range.parse_range_header(range_value, file_stat.st_size)) {
                                        LOG_WARN("Invalid Range header: %s", range_value);
                                    }
                                }
                            }
                        }
                    }

                    // 配置下载参数
                    AdvancedDownloadConfig download_config;
                    download_config.enable_rate_limit = false;
                    download_config.chunk_size = 128 * 1024;  // 128KB
                    download_config.progress_interval = 1 * 1024 * 1024;  // 1MB

                    // 进度回调
                    auto progress_cb = [connfd](const DownloadProgress& progress) {
                        static int last_percent = -1;
                        int current_percent = (int)progress.percentage;
                        if (current_percent % 10 == 0 && current_percent != last_percent) {
                            LOG_INFO("[fd=%d] Download progress: %.1f%% (%zu/%zu bytes, %.2f MB/s)",
                                     connfd, progress.percentage,
                                     progress.bytes_sent, progress.total_bytes,
                                     progress.download_rate / (1024.0 * 1024.0));
                            last_percent = current_percent;
                        }
                    };

                    fprintf(stderr, "[DEBUG] Before co_await handle_advanced_file_download\n");
                    fflush(stderr);

                    // 调用协程文件下载处理器
                    AdvancedDownloadResult download_result = co_await handle_advanced_file_download(
                        connfd,
                        m_io_uring_manager.get(),
                        file_path,
                        download_config,
                        progress_cb,
                        range.is_range_request ? &range : nullptr
                    );

                    fprintf(stderr, "[DEBUG] After co_await handle_advanced_file_download, success=%d\n",
                            download_result.success);
                    fflush(stderr);

                    if (download_result.success) {
                        LOG_INFO("File download completed: %s (%zu bytes, %.2fs)",
                                 download_result.filename.c_str(),
                                 download_result.bytes_sent,
                                 download_result.download_time_seconds);
                    } else {
                        LOG_ERROR("File download failed: %s", download_result.error_message.c_str());
                    }

                    // 下载处理完成，重置连接
                    conn->reset_connection();

                    // 根据是否 Keep-Alive 决定是否继续
                    if (!conn->get_linger()) {
                        break;
                    }
                    continue;  // 继续处理下一个请求
                } else {
                    // 3.4 对于非下载请求，调用完整的 process() 处理
                    conn->process();

                    // 4. 检查是否有响应需要发送
                    if (conn->get_bytes_to_send() == 0) {
                        LOG_WARN("No response to send for fd=%d", connfd);
                        break;
                    }
                }
            }

            // 5. 发送响应
            LOG_DEBUG("Coroutine: sending response to fd=%d, bytes=%d",
                      connfd, conn->get_bytes_to_send());

            // 5.1 发送响应头
            struct iovec* iv = conn->get_iovec();
            int iv_count = conn->get_iovec_count();

            if (iv_count > 0 && iv[0].iov_len > 0) {
                ssize_t bytes_written = co_await async_write(
                    m_io_uring_manager.get(),
                    connfd,
                    iv[0].iov_base,
                    iv[0].iov_len,
                    -1
                );

                LOG_DEBUG("Coroutine: wrote header %ld bytes to fd=%d", bytes_written, connfd);
                conn->add_bytes_have_send(bytes_written);
            }

            // 5.2 如果有文件内容，发送文件
            if (iv_count > 1 && iv[1].iov_len > 0) {
                size_t file_size = iv[1].iov_len;
                size_t file_sent = 0;
                const size_t CHUNK_SIZE = 128 * 1024;  // 128KB 分块

                // 检查是否使用 sendfile 模式
                if (conn->is_using_sendfile()) {
                    // sendfile 模式：需要从文件描述符读取数据
                    int file_fd = conn->get_file_fd();
                    std::unique_ptr<char[]> chunk_buffer(new char[CHUNK_SIZE]);

                    while (file_sent < file_size) {
                        size_t send_size = std::min(CHUNK_SIZE, file_size - file_sent);

                        // 使用 pread 读取文件内容
                        ssize_t read_bytes = pread(file_fd, chunk_buffer.get(), send_size, file_sent);
                        if (read_bytes <= 0) {
                            throw IoError("File read failed for sendfile mode");
                        }

                        // 写入网络
                        ssize_t bytes_written = co_await async_write(
                            m_io_uring_manager.get(),
                            connfd,
                            chunk_buffer.get(),
                            read_bytes,
                            -1
                        );

                        if (bytes_written <= 0) {
                            throw IoError("File write failed");
                        }

                        file_sent += bytes_written;
                        conn->add_bytes_have_send(bytes_written);

                        LOG_DEBUG("Coroutine: wrote file chunk %ld bytes to fd=%d (total=%zu/%zu)",
                                  bytes_written, connfd, file_sent, file_size);
                    }
                } else {
                    // mmap 模式：直接从内存写入
                    while (file_sent < file_size) {
                        size_t send_size = std::min(CHUNK_SIZE, file_size - file_sent);
                        char* file_base = (char*)iv[1].iov_base + file_sent;

                        ssize_t bytes_written = co_await async_write(
                            m_io_uring_manager.get(),
                            connfd,
                            file_base,
                            send_size,
                            -1
                        );

                        if (bytes_written <= 0) {
                            throw IoError("File write failed");
                        }

                        file_sent += bytes_written;
                        conn->add_bytes_have_send(bytes_written);

                        LOG_DEBUG("Coroutine: wrote file chunk %ld bytes to fd=%d (total=%zu/%zu)",
                                  bytes_written, connfd, file_sent, file_size);
                    }
                }
            }

            LOG_INFO("Coroutine: completed response for fd=%d, total=%d bytes",
                     connfd, conn->get_bytes_have_send());

            // 调整定时器
            if (timer_obj) {
                adjust_timer(timer_obj);
            }

            // 6. 检查是否保持连接
            if (!conn->get_linger()) {
                LOG_DEBUG("Connection not persistent, closing fd=%d", connfd);
                break;
            }

            // 重置连接状态，准备处理下一个请求
            conn->reset_connection();
            LOG_DEBUG("Connection reset for Keep-Alive: fd=%d", connfd);
        }
    }
    catch (const IoError& e) {
        LOG_ERROR("I/O error on fd=%d: %s (errno=%d)", connfd, e.what(), e.error_code);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception on fd=%d: %s", connfd, e.what());
    }

    // 清理：关闭连接并删除定时器
    LOG_INFO("Closing connection fd=%d", connfd);
    deal_timer(timer_obj, connfd);

    co_return;
}

/**
 * @brief 协程版本：持续接受新的客户端连接
 *
 * 这个协程负责：
 * 1. 循环等待新的客户端连接
 * 2. 接受连接后，为每个连接启动独立的处理协程
 * 3. 检查连接数限制
 *
 * @param scheduler 协程调度器，用于启动新的连接处理协程
 */
Task<void> WebServer::accept_connections_coro(AdvancedScheduler* scheduler)
{
    LOG_INFO("Starting accept coroutine on listen_fd=%d", m_listenfd);

    // 分配地址结构（重复使用）
    struct sockaddr_in client_address;
    socklen_t client_addrlen;

    try {
        // 主循环：持续接受新连接
        while (true) {
            // 重置地址结构
            bzero(&client_address, sizeof(client_address));
            client_addrlen = sizeof(client_address);

            // 异步接受新连接
            LOG_DEBUG("Accept coroutine: waiting for new connection...");

            int connfd = co_await async_accept(
                m_io_uring_manager.get(),
                m_listenfd,
                (struct sockaddr*)&client_address,
                &client_addrlen
            );

            if (connfd < 0) {
                LOG_ERROR("Accept failed: %d", connfd);
                continue;
            }

            LOG_INFO("Accepted connection fd=%d from %s:%d",
                     connfd,
                     inet_ntoa(client_address.sin_addr),
                     ntohs(client_address.sin_port));

            // 检查连接数限制
            if (m_user_manager->get_user_count() >= MAX_FD) {
                LOG_ERROR("Server busy: too many connections (%d >= %d)",
                          m_user_manager->get_user_count(), MAX_FD);
                utils.show_error(connfd, "Internal server busy");
                close(connfd);
                continue;
            }

            // 为新连接启动处理协程（NORMAL优先级）
            bool spawned = scheduler->spawn(
                handle_http_connection_coro(connfd, client_address),
                CoroutinePriority::NORMAL,
                "http-handler"
            );

            if (!spawned) {
                LOG_ERROR("Failed to spawn handler for fd=%d (scheduler full)", connfd);
                utils.show_error(connfd, "Server overloaded");
                close(connfd);
            }
        }
    }
    catch (const IoError& e) {
        LOG_ERROR("Accept error: %s (errno=%d)", e.what(), e.error_code);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Accept exception: %s", e.what());
    }

    LOG_WARN("Accept coroutine terminated");
    co_return;
}

/**
 * @brief 协程版本的事件循环
 *
 * 使用协程调度器管理所有 I/O 操作：
 * 1. 创建协程调度器
 * 2. 启动 accept 协程（负责接受新连接）
 * 3. 运行调度器（自动调度所有协程）
 *
 * 优势：
 * - 代码清晰：不需要手动管理状态机
 * - 易于维护：每个连接的逻辑都在独立的协程中
 * - 异常安全：协程自动处理异常和清理
 */
void WebServer::eventLoop_coro()
{
    if (!m_io_uring_manager || !m_io_uring_manager->is_initialized())
    {
        LOG_ERROR("io_uring not initialized, cannot start coroutine event loop");
        return;
    }

    if (!m_coro_scheduler)
    {
        LOG_ERROR("Advanced scheduler not initialized");
        return;
    }

    LOG_INFO("Starting advanced coroutine event loop (queue depth=%u)",
             m_io_uring_manager->get_queue_depth());

    // 启动 accept 协程（高优先级）
    bool spawned = m_coro_scheduler->spawn(
        accept_connections_coro(m_coro_scheduler.get()),
        CoroutinePriority::HIGH,
        "accept-loop"
    );

    if (!spawned)
    {
        LOG_ERROR("Failed to spawn accept coroutine");
        return;
    }

    LOG_INFO("Accept coroutine spawned with HIGH priority");

    // 运行调度器
    LOG_INFO("Running advanced coroutine scheduler...");
    m_coro_scheduler->run();

    // 打印统计信息
    LOG_INFO("Coroutine event loop stopped, printing statistics:");
    m_coro_scheduler->print_stats();
    m_cpu_thread_pool->print_stats();
    m_task_dispatcher->print_stats();
    get_global_coro_monitor().print_dashboard();
}
#endif  // USE_COROUTINE

// ==================== io_uring 回调实现 ====================

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

