#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 引入新的管理器类
#include "../epoll/epoll_manager.h"
#include "../user/user_manager.h"
#include "../cache/static_cache.h"

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 【已移除】全局变量 m_lock 和 users
// 这些现在由 UserManager 管理

// 构造函数
http_conn::http_conn()
    : m_epoll_manager(nullptr), m_user_manager(nullptr), m_static_cache(nullptr)
{
}

// 析构函数
http_conn::~http_conn()
{
}

// 依赖注入：设置 EpollManager
void http_conn::set_epoll_manager(EpollManager *epoll_mgr)
{
    m_epoll_manager = epoll_mgr;
}

// 依赖注入：设置 UserManager
void http_conn::set_user_manager(UserManager *user_mgr)
{
    m_user_manager = user_mgr;
}

// 依赖注入：设置 StaticCache
void http_conn::set_static_cache(StaticCache *cache)
{
    m_static_cache = cache;
}

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql_conn = NULL;
    connectionRAII mysqlcon(&mysql_conn, connPool);

    //在user表中检索username,passwd数据,浏览器端输入
    if (mysql_query(mysql_conn, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql_conn));
        return;
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql_conn);
    if (!result)
    {
        LOG_ERROR("mysql_store_result error:%s\n", mysql_error(mysql_conn));
        return;
    }

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 构建用户数据map
    map<string, string> users_data;
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users_data[temp1] = temp2;
    }

    // 将用户数据存储到 UserManager（所有http_conn共享）
    if (m_user_manager)
    {
        m_user_manager->set_users(users_data);
    }

    mysql_free_result(result);
}

// 【已移除】全局函数 setnonblocking, addfd, removefd, modfd
// 这些现在由 EpollManager 提供

// 【已移除】静态成员变量初始化
// int http_conn::m_user_count = 0;
// int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);

        // 使用 EpollManager 移除 fd
        if (m_epoll_manager)
        {
            m_epoll_manager->removefd(m_sockfd);
        }

        m_sockfd = -1;

        // 使用 UserManager 减少用户计数
        if (m_user_manager)
        {
            m_user_manager->decrement_user_count();
        }
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 使用 EpollManager 添加 fd
    if (m_epoll_manager)
    {
        m_epoll_manager->addfd(sockfd, true, TRIGMode);
    }

    // 使用 UserManager 增加用户计数
    if (m_user_manager)
    {
        m_user_manager->increment_user_count();
    }

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    // 零拷贝优化初始化
    m_file_fd = -1;
    m_use_sendfile = false;
    m_use_cache = false;

    // HTTP请求头初始化
    m_if_none_match = nullptr;
    m_accept_encoding = nullptr;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "If-None-Match:", 14) == 0)
    {
        text += 14;
        text += strspn(text, " \t");
        m_if_none_match = text;
    }
    else if (strncasecmp(text, "Accept-Encoding:", 16) == 0)
    {
        text += 16;
        text += strspn(text, " \t");
        m_accept_encoding = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        // 优化：使用栈内存，避免 malloc/free
        char url_buffer[256];
        snprintf(url_buffer, sizeof(url_buffer), "/%s", m_url + 2);
        strncpy(m_real_file + len, url_buffer, FILENAME_LEN - len - 1);

        //将用户名和密码提取出来 - 带边界检查防止缓冲区溢出
        //user=123&passwd=123
        const int MAX_FIELD_LEN = 99;
        char name[100], password[100];
        int i, j = 0;

        // 解析用户名: user=<name>&
        for (i = 5; m_string[i] != '&' && m_string[i] != '\0' && j < MAX_FIELD_LEN; ++i, ++j)
            name[j] = m_string[i];
        name[j] = '\0';

        // 检查解析是否成功
        if (m_string[i] != '&' || j == 0 || j >= MAX_FIELD_LEN) {
            LOG_ERROR("Invalid username format: too long or malformed");
            strcpy(m_url, "/registerError.html");
            return do_request();  // 直接返回错误页面
        }

        // 解析密码: passwd=<password>
        i += 8;  // 跳过 "&passwd="
        j = 0;
        for (; m_string[i] != '\0' && m_string[i] != '&' && j < MAX_FIELD_LEN; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 检查密码
        if (j == 0 || j >= MAX_FIELD_LEN) {
            LOG_ERROR("Invalid password format: too long or empty");
            strcpy(m_url, "/registerError.html");
            return do_request();
        }

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据

            // 使用 UserManager 检查用户是否存在
            if (m_user_manager && !m_user_manager->has_user(string(name)))
            {
                // 检查 mysql 连接是否有效
                if (!mysql)
                {
                    LOG_ERROR("MySQL connection is NULL during registration");
                    strcpy(m_url, "/registerError.html");
                }
                else
                {
                    // 使用 MySQL 预处理语句防止 SQL 注入
                    MYSQL_STMT *stmt = mysql_stmt_init(mysql);
                    if (!stmt)
                    {
                        LOG_ERROR("mysql_stmt_init failed");
                        strcpy(m_url, "/registerError.html");
                    }
                    else
                    {
                        const char *query = "INSERT INTO user(username, passwd) VALUES(?, ?)";
                        if (mysql_stmt_prepare(stmt, query, strlen(query)) != 0)
                        {
                            LOG_ERROR("mysql_stmt_prepare failed: %s", mysql_stmt_error(stmt));
                            mysql_stmt_close(stmt);
                            strcpy(m_url, "/registerError.html");
                        }
                        else
                        {
                            // 绑定参数
                            MYSQL_BIND bind[2];
                            memset(bind, 0, sizeof(bind));

                            unsigned long name_len = strlen(name);
                            unsigned long password_len = strlen(password);

                            bind[0].buffer_type = MYSQL_TYPE_STRING;
                            bind[0].buffer = name;
                            bind[0].buffer_length = name_len;
                            bind[0].length = &name_len;

                            bind[1].buffer_type = MYSQL_TYPE_STRING;
                            bind[1].buffer = password;
                            bind[1].buffer_length = password_len;
                            bind[1].length = &password_len;

                            if (mysql_stmt_bind_param(stmt, bind) != 0)
                            {
                                LOG_ERROR("mysql_stmt_bind_param failed: %s", mysql_stmt_error(stmt));
                                mysql_stmt_close(stmt);
                                strcpy(m_url, "/registerError.html");
                            }
                            else
                            {
                                // 执行并获取锁
                                locker &lock = m_user_manager->get_lock();
                                lock.lock();

                                int res = mysql_stmt_execute(stmt);
                                // 使用不加锁的版本避免死锁
                                if (res == 0)
                                {
                                    m_user_manager->add_user_unlocked(string(name), string(password));
                                }

                                lock.unlock();
                                mysql_stmt_close(stmt);

                                if (res == 0)
                                    strcpy(m_url, "/log.html");
                                else
                                {
                                    LOG_ERROR("mysql_stmt_execute failed: %s", mysql_error(mysql));
                                    strcpy(m_url, "/registerError.html");
                                }
                            }
                        }
                    }
                }
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            string stored_password;
            // 使用 UserManager 查找用户
            if (m_user_manager &&
                m_user_manager->find_user(string(name), stored_password) &&
                stored_password == string(password))
            {
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    // 优化：使用栈内存统一处理路由，避免多次 malloc/free
    const char *target_page = NULL;
    if (*(p + 1) == '0')
        target_page = "/register.html";
    else if (*(p + 1) == '1')
        target_page = "/log.html";
    else if (*(p + 1) == '5')
        target_page = "/picture.html";
    else if (*(p + 1) == '6')
        target_page = "/video.html";
    else if (*(p + 1) == '7')
        target_page = "/fans.html";

    if (target_page)
        strncpy(m_real_file + len, target_page, FILENAME_LEN - len - 1);
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 静态内容缓存优化：对于GET请求尝试从缓存获取
    if (m_static_cache && m_method == GET && cgi == 0)
    {
        std::shared_ptr<CacheEntry> cached = m_static_cache->get(std::string(m_real_file));
        if (cached && cached->last_modified == m_file_stat.st_mtime)
        {
            // 保存ETag用于响应
            m_cached_etag = cached->etag;

            // 检查ETag（304 Not Modified）
            if (m_if_none_match && strcmp(m_if_none_match, cached->etag.c_str()) == 0)
            {
                // ETag匹配，返回304（将在process_write中处理）
                m_use_cache = true;
                return FILE_REQUEST;
            }

            // 使用缓存内容
            m_use_cache = true;
            m_file_address = cached->data.get();
            m_file_fd = -1;
            m_use_sendfile = false;
            return FILE_REQUEST;
        }
    }

    // 零拷贝优化：保持文件描述符打开，用于 sendfile
    m_file_fd = open(m_real_file, O_RDONLY);
    if (m_file_fd < 0)
        return NO_RESOURCE;

    // 根据文件大小决定使用 sendfile 还是 mmap
    // sendfile 对大文件更高效（避免用户空间拷贝）
    const off_t SENDFILE_THRESHOLD = 64 * 1024; // 64KB 阈值
    const off_t CACHE_THRESHOLD = 512 * 1024;    // 512KB以下才缓存

    if (m_file_stat.st_size >= SENDFILE_THRESHOLD)
    {
        // 使用 sendfile 零拷贝优化（大文件）
        m_use_sendfile = true;
        m_file_address = NULL; // 不需要 mmap
    }
    else
    {
        // 使用传统 mmap 方式（小文件，mmap 开销可接受）
        m_use_sendfile = false;
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, m_file_fd, 0);
        if (m_file_address == MAP_FAILED)
        {
            close(m_file_fd);
            m_file_fd = -1;
            return NO_RESOURCE;
        }

        // 加入缓存（仅对小文件且GET请求）
        if (m_static_cache && m_method == GET && cgi == 0 &&
            m_file_stat.st_size <= CACHE_THRESHOLD)
        {
            m_static_cache->put(std::string(m_real_file),
                                m_file_address,
                                m_file_stat.st_size,
                                m_file_stat.st_mtime);

            // 获取刚放入缓存的ETag
            auto cached = m_static_cache->get(std::string(m_real_file));
            if (cached)
            {
                m_cached_etag = cached->etag;
            }
        }

        // 小文件可以关闭 fd，mmap 已经建立映射
        close(m_file_fd);
        m_file_fd = -1;
    }

    return FILE_REQUEST;
}

void http_conn::unmap()
{
    // 清理 mmap 映射（仅当不使用缓存时）
    if (m_file_address && !m_use_cache)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }

    // 清理 sendfile 文件描述符
    if (m_file_fd >= 0)
    {
        close(m_file_fd);
        m_file_fd = -1;
    }

    m_use_sendfile = false;
    m_use_cache = false;
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        // 使用 EpollManager 修改 fd 事件
        if (m_epoll_manager)
        {
            m_epoll_manager->modfd(m_sockfd, EPOLLIN, m_TRIGMode);
        }
        init();
        return true;
    }

    // 零拷贝优化：使用 sendfile 发送大文件
    if (m_use_sendfile && m_file_fd >= 0)
    {
        while (1)
        {
            // 第一步：发送 HTTP 响应头（如果还未发送完）
            if (m_write_idx > 0 && bytes_have_send < m_write_idx)
            {
                temp = send(m_sockfd, m_write_buf + bytes_have_send, m_write_idx - bytes_have_send, 0);
                if (temp < 0)
                {
                    if (errno == EAGAIN)
                    {
                        if (m_epoll_manager)
                        {
                            m_epoll_manager->modfd(m_sockfd, EPOLLOUT, m_TRIGMode);
                        }
                        return true;
                    }
                    unmap();
                    return false;
                }
                bytes_have_send += temp;
                bytes_to_send -= temp;
                continue; // 继续发送剩余的响应头
            }

            // 第二步：使用 sendfile 零拷贝发送文件内容
            off_t offset = bytes_have_send - m_write_idx; // 文件中的偏移量
            off_t remaining = m_file_stat.st_size - offset;

            if (remaining > 0)
            {
                temp = sendfile(m_sockfd, m_file_fd, &offset, remaining);
                if (temp < 0)
                {
                    if (errno == EAGAIN)
                    {
                        // sendfile 会自动更新 offset，需要同步
                        bytes_have_send = m_write_idx + offset;
                        bytes_to_send = m_file_stat.st_size - offset;
                        if (m_epoll_manager)
                        {
                            m_epoll_manager->modfd(m_sockfd, EPOLLOUT, m_TRIGMode);
                        }
                        return true;
                    }
                    unmap();
                    return false;
                }
                else if (temp == 0)
                {
                    // 发送完成
                    break;
                }

                // sendfile 自动更新了 offset
                bytes_have_send = m_write_idx + offset;
                bytes_to_send -= temp;
            }
            else
            {
                // 文件发送完成
                break;
            }
        }

        // 发送完成
        unmap();
        if (m_epoll_manager)
        {
            m_epoll_manager->modfd(m_sockfd, EPOLLIN, m_TRIGMode);
        }

        if (m_linger)
        {
            init();
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        // 传统方式：使用 writev 发送（mmap + writev）
        while (1)
        {
            temp = writev(m_sockfd, m_iv, m_iv_count);

            if (temp < 0)
            {
                if (errno == EAGAIN)
                {
                    // 使用 EpollManager 修改 fd 事件
                    if (m_epoll_manager)
                    {
                        m_epoll_manager->modfd(m_sockfd, EPOLLOUT, m_TRIGMode);
                    }
                    return true;
                }
                unmap();
                return false;
            }

            bytes_have_send += temp;
            bytes_to_send -= temp;
            if (bytes_have_send >= m_iv[0].iov_len)
            {
                m_iv[0].iov_len = 0;
                m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
                m_iv[1].iov_len = bytes_to_send;
            }
            else
            {
                m_iv[0].iov_base = m_write_buf + bytes_have_send;
                m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
            }

            if (bytes_to_send <= 0)
            {
                unmap();
                // 使用 EpollManager 修改 fd 事件
                if (m_epoll_manager)
                {
                    m_epoll_manager->modfd(m_sockfd, EPOLLIN, m_TRIGMode);
                }

                if (m_linger)
                {
                    init();
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    if (!add_content_type_auto())  // 自动识别Content-Type
        return false;
    if (!add_content_length(content_len))
        return false;
    if (!add_linger())
        return false;

    // 对于静态资源添加缓存控制
    if (m_method == GET && cgi == 0)
    {
        // Cache-Control: 静态资源缓存1小时
        add_cache_control("public, max-age=3600");

        // 如果有ETag，添加到响应头
        if (!m_cached_etag.empty())
        {
            add_etag(m_cached_etag.c_str());
        }
    }

    return add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据文件扩展名获取MIME类型
const char* http_conn::get_mime_type(const char* filename)
{
    const char* ext = strrchr(filename, '.');
    if (!ext)
        return "application/octet-stream";

    // 常见MIME类型映射
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    else if (strcmp(ext, ".css") == 0)
        return "text/css";
    else if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    else if (strcmp(ext, ".json") == 0)
        return "application/json";
    else if (strcmp(ext, ".xml") == 0)
        return "application/xml";
    else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    else if (strcmp(ext, ".png") == 0)
        return "image/png";
    else if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    else if (strcmp(ext, ".svg") == 0)
        return "image/svg+xml";
    else if (strcmp(ext, ".ico") == 0)
        return "image/x-icon";
    else if (strcmp(ext, ".txt") == 0)
        return "text/plain";
    else if (strcmp(ext, ".pdf") == 0)
        return "application/pdf";
    else if (strcmp(ext, ".mp4") == 0)
        return "video/mp4";
    else if (strcmp(ext, ".mp3") == 0)
        return "audio/mpeg";
    else
        return "application/octet-stream";
}

// 自动识别Content-Type
bool http_conn::add_content_type_auto()
{
    const char* mime_type = get_mime_type(m_real_file);
    return add_response("Content-Type:%s\r\n", mime_type);
}

bool http_conn::add_linger()
{
    if (m_linger)
    {
        // Keep-Alive: timeout=30秒, max=1000个请求
        return add_response("Connection: keep-alive\r\nKeep-Alive: timeout=30, max=1000\r\n");
    }
    else
    {
        return add_response("Connection: close\r\n");
    }
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::add_etag(const char *etag)
{
    return add_response("ETag:%s\r\n", etag);
}

bool http_conn::add_cache_control(const char *directive)
{
    return add_response("Cache-Control:%s\r\n", directive);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        // 检查是否应返回 304 Not Modified
        if (m_if_none_match && !m_cached_etag.empty() &&
            strcmp(m_if_none_match, m_cached_etag.c_str()) == 0)
        {
            // ETag匹配，返回304
            add_status_line(304, "Not Modified");
            // 304响应只需要部分头部
            add_content_type_auto();
            add_cache_control("public, max-age=3600");
            add_etag(m_cached_etag.c_str());
            add_blank_line();

            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv_count = 1;
            bytes_to_send = m_write_idx;
            return true;
        }

        // 正常200响应
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        // 使用 EpollManager 修改 fd 事件
        if (m_epoll_manager)
        {
            m_epoll_manager->modfd(m_sockfd, EPOLLIN, m_TRIGMode);
        }
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    // 使用 EpollManager 修改 fd 事件
    if (m_epoll_manager)
    {
        m_epoll_manager->modfd(m_sockfd, EPOLLOUT, m_TRIGMode);
    }
}
