#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "./sql_connection_pool.h"

void con_pool::init(string url, string user, string password, string dbname, int port, unsigned int maxcon)
{
    // 初始化信息
    this->m_url = url;
    this->m_database_name = dbname;
    this->m_password = password;
    this->m_port = port;
    this->max_conn = maxcon;
    this->m_user = user;

    for (int i = 0; i < max_conn; i++)
    {
        MYSQL *con = nullptr;
        con = mysql_init(con);
        if (con == nullptr)
        {
            cout << "Error:" << mysql_error(con);
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), dbname.c_str(), port, NULL, 0);

        conn_list.push_back(con);
        free_conn++;
    }
    this->max_conn = free_conn;
    reserve = sem(max_conn);
}

MYSQL *con_pool::get_conn()
{
    if (conn_list.size() == 0)
    {
        return nullptr;
    }
    MYSQL *conn = nullptr;
    reserve.wait();
    lock.lock();

    conn = conn_list.front();
    conn_list.pop_front();
    lock.unlock();
    return conn;
}

void con_pool::destory_conn()
{
    lock.lock();
    if (conn_list.size() > 0)
    {
        for (auto it = conn_list.begin(); it != conn_list.end(); it++)
        {
            mysql_close(*it);
        }
        max_conn = 0;
        conn_list.clear();
    }
    lock.unlock();
}

bool con_pool::release_conn(MYSQL *conn)
{
    if (conn == nullptr)
    {
        return false;
    }
    lock.lock();

    conn_list.push_back(conn);
    lock.unlock();

    reserve.post();
    return true;
}

con_pool *con_pool::get_instance()
{
    static con_pool pool;
    return &pool;
}

con_pool::con_pool()
{
}

con_pool::~con_pool()
{
    destory_conn();
}
conn_rall::conn_rall(MYSQL **sql, con_pool *pool)
{
    *sql = pool->get_conn();
    m_sql = *sql;
    m_con_pool = pool;
}
conn_rall::~conn_rall()
{
    m_con_pool->release_conn(m_sql);
}