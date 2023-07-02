// 线程同步机制包装类
// ===============
// 多线程同步，确保任一时刻只能有一个线程能进入关键代码段.
// > * 信号量
// > * 互斥锁/互斥量
// > * 条件变量
#ifndef LOCKER_H
#define LOCKER_H
#include <exception>
#include <semaphore.h>
#include <pthread.h>
#include <stdexcept>
using namespace std;
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {

            throw new logic_error("sem初始化失败");
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    sem(int num)
    {

        if (sem_init(&m_sem, 0, num) != 0)
        {

            throw new logic_error("sem初始化失败");
        }
    }
    bool wait()
    {
        return sem_wait(&m_sem);
    }
    bool post()
    {
        return sem_post(&m_sem);
    }

private:
    sem_t m_sem;
};
class locker
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex);
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex);
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};
class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond);
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond);
    }
    bool wait(pthread_mutex_t *mutex)
    {
        int ret = 0;
        // 调用此函数后，会阻塞当前线程，并且等待条件变量信号，释放互斥锁
        // 当获得信号后，重新获取互斥量后才能唤醒线程执行。
        ret = pthread_cond_wait(&m_cond, mutex);
        return ret == 0;
    }
    bool wait_time(pthread_mutex_t *mutex, timespec time)
    {
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, mutex, &time);
        return ret == 0;
    }

private:
    pthread_cond_t m_cond;
};
#endif