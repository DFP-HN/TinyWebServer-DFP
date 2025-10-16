#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <map>
#include <string>
#include "../lock/locker.h"

using namespace std;

// 用户管理器 - 封装用户数据和用户计数
// 替代 http_conn 中的全局变量和静态成员
class UserManager
{
public:
    UserManager();
    ~UserManager();

    // 用户计数管理
    int get_user_count() const { return m_user_count; }
    void increment_user_count();
    void decrement_user_count();

    // 用户数据管理（从数据库加载的用户名密码映射）
    void set_users(const map<string, string> &users);
    map<string, string> &get_users();
    const map<string, string> &get_users() const;

    // 线程安全的用户查找
    bool find_user(const string &username, string &password) const;
    bool has_user(const string &username) const;
    bool add_user(const string &username, const string &password);

    // 获取锁（用于数据库操作）
    locker &get_lock() { return m_lock; }

private:
    int m_user_count;                // 当前连接的用户数
    map<string, string> m_users;     // 从数据库加载的用户数据
    locker m_lock;                   // 保护共享数据的互斥锁
};

#endif
