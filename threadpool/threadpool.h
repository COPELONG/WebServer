#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
template <typename T>
class threadpool
{

public:
    threadpool(con_pool *connPool, int m_thread_num = 8, int m_max_request = 10000);
    ~threadpool();

    bool append(T *request);

private:
    static void *worker(void *arg);
    void run();

private:
    locker m_queuelocker;
    pthread_t *m_threads;
    list<T *> m_workqueue;
    sem m_queuestat;
    bool m_stop;
    int m_thread_num;
    int m_max_request;
    con_pool *m_connPool; // 数据库
};
template <typename T>
threadpool<T>::threadpool(con_pool *connPool, int thread_num, int max_request) : m_thread_num(thread_num), m_max_request(max_request), m_stop(false), m_threads(NULL), m_connPool(connPool)
{
    if (thread_num <= 0 || max_request < -0)
    {
        throw exception();
    }
    m_threads = new pthread_t[thread_num];
    for (int i = 0; i < thread_num; i++)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_request)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelocker.unlock();
        conn_rall(&request->mysql, m_connPool);
        request->process();
    }
}
#endif