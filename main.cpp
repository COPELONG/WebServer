#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"
using namespace std;

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大事件数
#define TIMESLOT 5             // 最小超时单位

// #define listenfdET //边缘触发非阻塞
#define listenfdLT // 水平触发阻塞

#define SYNLOG // 同步写日志
// #define ASYNLOG //异步写日志

// 这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int set_unblock(int fd);

// 设置定时器相关参数
static int pipefd[2];
static int epollfd;
static sort_timer_lst timer_lst;
// 定时处理任务,重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(SIGALRM);
}
// 定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    http_conn::m_users_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}
// 设置信号处理函数
void sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
// 设置信号函数，对某一个信号进行屏蔽等处理
void addsig(int sig, void (*handler)(int), bool restart = true)
{

    struct sigaction sa;
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask); // 将所有信号添加到信号集中
    assert(sigaction(sig, &sa, NULL) != -1);
}
// 向客户端发送错误信息，并在发送后关闭连接。
void show_error(int connfd, const char *info)
{
    std::cout << "info" << endl;
    send(connfd, info, sizeof(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{

#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); // 异步日志模型
#endif
#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); // 同步日志模型
#endif
    if (argc <= 1)
    {
        cout << "输入参数不符合要求：./xxx num" << endl;
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    con_pool *connPool = con_pool::get_instance();
    // 初始化数据连接池。
    connPool->init("localhost", "root", "12345678", "mydb", 3306, 8);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];
    assert(users); // 断言，检查指针是否为空、

    // 初始化数据库读取表:  获取数据库表中的账号和密码，存储在MAP集合中
    users->mysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    // 端口复用，程序再次连接仍然可以绑定相同的地址和端口

    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));

    assert(ret >= 0);

    ret = listen(listenfd, 5);

    assert(ret >= 0);

    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    http_conn::m_epollfd = epollfd;
    addfd(epollfd, listenfd, false);

    // 创建管道

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    set_unblock(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;
    // 超时标志
    bool timeout = false;

    // 每隔TIMESLOT时间触发SIGALRM信号    5S
    alarm(TIMESLOT);
    // 创建拥有定时器的客户端函数

    client_data *client_users = new client_data[MAX_FD];

    while (!stop_server)
    {

        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        if (num < 0 && errno != EINTR)
        {
            std::cout << "进入日志" << endl;
            LOG_ERROR("%s", "epoll failure");
            std::cout << "跳出循环" << endl;
            break;
        }

        for (int i = 0; i < num; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in cilent_address;
                socklen_t cilent_length = sizeof(cilent_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&cilent_address, &cilent_length);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }

                if (http_conn::m_users_count >= MAX_FD)
                {
                    show_error(connfd, "服务器繁忙：当前无可用描述符");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }

                users[connfd].init(connfd, cilent_address);
                // 定时器相关设置
                std::cout << "212 yes" << endl;
                util_timer *timer = new util_timer;
                client_users[connfd].address = cilent_address;
                client_users[connfd].sockfd = connfd;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                timer->cb_func = cb_func;
                std::cout << "219 yes" << endl;
                timer->user_data = &client_users[connfd];
                std::cout << "221 yes" << endl;
                client_users[connfd].timer = timer;
                std::cout << "223 yes" << endl;
                timer_lst.add_timer(timer);
                std::cout << "225 yes" << endl;

#endif

#ifdef listenET
                while (true)
                {
                    int connfd = accept(epollfd, (struct sockaddr *)&cilent_address, &cilent_length);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        continue;
                    }
                    if (http_conn::m_users_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        continue;
                    }
                    users[connfd].init(connfd, cilent_address);
                    // 初始化client_data数据
                    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }

#endif
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = client_users[sockfd].timer;
                timer->cb_func(&client_users[sockfd]);
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
            // 处理信号

            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig = 0;
                char sigals[1024];
                // 正常情况下，这里的ret返回值总是1，
                // 只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], sigals, sizeof(sigals), 0);
                if (ret == 0 || ret == -1)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; i++)
                    {
                        switch (sigals[i])
                        {
                        case SIGALRM:
                            timeout = true;
                            break;
                        case SIGTERM:
                            stop_server = true;
                        }
                    }
                }
            }

            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                std::cout << "299 yes" << endl;
                util_timer *timer = client_users[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    std::cout << "303 yes" << endl;
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // 读取成功，加入工作线程中。
                    pool->append(users + sockfd);
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    // 关闭服务器
                    timer->cb_func(&client_users[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = client_users[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    // 关闭连接
                    timer->cb_func(&client_users[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] client_users;
    delete pool;
    delete[] users;
    return 0;
}
