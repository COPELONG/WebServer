#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

class con_pool
{
public:
    MYSQL *get_conn();             // 获取数据库连接
    bool release_conn(MYSQL *con); // 释放连接
    int get_free_conn();           // 获取连接
    void destory_conn();           // 销毁所有连接

    static con_pool *get_instance();
    void init(string url, string user, string password, string dataname, int post, unsigned int maxconn);

private:
    con_pool();
    ~con_pool();

private:
    unsigned int cur_conn;
    unsigned int max_conn;
    unsigned int free_conn;

private:
    locker lock;
    list<MYSQL *> conn_list;
    sem reserve;

private:
    string m_url;
    string m_port;
    string m_user;
    string m_password;
    string m_database_name;
};

class conn_rall
{
public:
    conn_rall(MYSQL **sql, con_pool *pool);
    ~conn_rall();

private:
    MYSQL *m_sql;
    con_pool *m_con_pool;
};

#endif