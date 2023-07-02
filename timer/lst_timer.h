#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer;
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer
{

public:
    util_timer() : prev(NULL), next(NULL)
    {
    }

public:
    client_data *user_data;
    time_t expire;
    void (*cb_func)(client_data *);
    util_timer *prev;
    util_timer *next;
};

class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}

    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    void add_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        if (!head)
        {
            head = tail = timer;
            return;
        }
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }
    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        if (!tmp || timer->expire < tmp->expire)
        {
            return;
        }
        if (timer == head)
        {
            head = head->next;
            timer->next = NULL;
            head->prev = NULL;
            add_timer(timer, head);
        }
        else
        {
            timer->next->prev = timer->prev;
            timer->prev->next = timer->next;
            add_timer(timer, timer->next);
        }
    }
    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        if (timer == head && timer == tail)
        {
            head = tail = NULL;
            delete timer;
            return;
        }
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    // 从头结点开始遍历,SIGALRM信号每次被触发，
    // 主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器。
    void tick()
    {
        if (!head)
        {
            return;
        }
        util_timer *tmp = head;
        time_t cur = time(NULL);
        while (tmp)
        {
            if (cur < tmp->expire)
            {
                break;
            }
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            tmp->cb_func(tmp->user_data);
            delete tmp;
            tmp = head;
        }
    }

private:
    void add_timer(util_timer *timer, util_timer *head)
    {
        util_timer *prev = head;
        util_timer *tmp = prev->next;
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                timer->next = tmp;
                timer->prev = prev;
                prev->next = timer;
                tmp->prev = timer;
                break;
            }
            prev = tmp;
            tmp = prev->next;
        }
        if (!tmp)
        {
            tail->next = timer;
            timer->prev = tail;
            tail = timer;
            timer->next = NULL;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif