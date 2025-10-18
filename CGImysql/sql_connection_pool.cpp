#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	// 原子变量初始化
	m_CurConn.store(0, std::memory_order_relaxed);
	m_FreeConn.store(0, std::memory_order_relaxed);
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL init Error: %s", mysql_error(con));
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL connection Error: %s (host=%s, user=%s, db=%s, port=%d)",
				mysql_error(con), url.c_str(), User.c_str(), DBName.c_str(), Port);
			exit(1);
		}
		connList.push_back(con);
		// 原子操作：无锁增加空闲连接数
		m_FreeConn.fetch_add(1, std::memory_order_relaxed);
	}

	reserve = sem(m_FreeConn.load(std::memory_order_relaxed));

	m_MaxConn = m_FreeConn.load(std::memory_order_relaxed);
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();

	// 优化：锁只保护队列操作，计数器使用原子操作
	lock.lock();
	con = connList.front();
	connList.pop_front();
	lock.unlock();

	// 原子操作：无锁更新计数器（性能提升关键点）
	m_FreeConn.fetch_sub(1, std::memory_order_relaxed);
	m_CurConn.fetch_add(1, std::memory_order_relaxed);

	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	// 优化：锁只保护队列操作
	lock.lock();
	connList.push_back(con);
	lock.unlock();

	// 原子操作：无锁更新计数器
	m_FreeConn.fetch_add(1, std::memory_order_relaxed);
	m_CurConn.fetch_sub(1, std::memory_order_relaxed);

	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		// 原子操作：重置计数器
		m_CurConn.store(0, std::memory_order_relaxed);
		m_FreeConn.store(0, std::memory_order_relaxed);
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	// 原子操作：无锁读取（性能提升关键点）
	// 之前每次查询都需要加锁，现在直接原子读取
	return this->m_FreeConn.load(std::memory_order_relaxed);
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}