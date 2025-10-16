#include "user_manager.h"

UserManager::UserManager() : m_user_count(0)
{
    pthread_rwlock_init(&m_rwlock, NULL);
}

UserManager::~UserManager()
{
    pthread_rwlock_destroy(&m_rwlock);
}

void UserManager::increment_user_count()
{
    m_lock.lock();
    ++m_user_count;
    m_lock.unlock();
}

void UserManager::decrement_user_count()
{
    m_lock.lock();
    --m_user_count;
    m_lock.unlock();
}

void UserManager::set_users(const map<string, string> &users)
{
    pthread_rwlock_wrlock(&m_rwlock);
    m_users = users;
    pthread_rwlock_unlock(&m_rwlock);
}

map<string, string> &UserManager::get_users()
{
    return m_users;
}

const map<string, string> &UserManager::get_users() const
{
    return m_users;
}

bool UserManager::find_user(const string &username, string &password) const
{
    // 使用读锁：允许多个线程同时查找用户（提升并发性能）
    pthread_rwlock_rdlock(&m_rwlock);
    map<string, string>::const_iterator it = m_users.find(username);
    bool found = false;
    if (it != m_users.end())
    {
        password = it->second;
        found = true;
    }
    pthread_rwlock_unlock(&m_rwlock);
    return found;
}

bool UserManager::has_user(const string &username) const
{
    // 使用读锁：允许多个线程同时查询
    pthread_rwlock_rdlock(&m_rwlock);
    bool exists = m_users.find(username) != m_users.end();
    pthread_rwlock_unlock(&m_rwlock);
    return exists;
}

bool UserManager::add_user(const string &username, const string &password)
{
    // 使用写锁
    pthread_rwlock_wrlock(&m_rwlock);
    if (m_users.find(username) == m_users.end())
    {
        m_users[username] = password;
        pthread_rwlock_unlock(&m_rwlock);
        return true;
    }
    pthread_rwlock_unlock(&m_rwlock);
    return false;
}

// 不加锁的版本，供已持有锁的调用者使用
void UserManager::add_user_unlocked(const string &username, const string &password)
{
    m_users[username] = password;
}
