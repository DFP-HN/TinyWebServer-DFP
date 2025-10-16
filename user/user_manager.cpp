#include "user_manager.h"

UserManager::UserManager() : m_user_count(0)
{
}

UserManager::~UserManager()
{
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
    m_lock.lock();
    m_users = users;
    m_lock.unlock();
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
    map<string, string>::const_iterator it = m_users.find(username);
    if (it != m_users.end())
    {
        password = it->second;
        return true;
    }
    return false;
}

bool UserManager::has_user(const string &username) const
{
    return m_users.find(username) != m_users.end();
}

bool UserManager::add_user(const string &username, const string &password)
{
    m_lock.lock();
    if (m_users.find(username) == m_users.end())
    {
        m_users[username] = password;
        m_lock.unlock();
        return true;
    }
    m_lock.unlock();
    return false;
}
