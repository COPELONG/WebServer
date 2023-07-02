#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}
Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

bool Log::init(const char *file_name, int log_buf_size, int split_lines, int max_queue_size)
{
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);

        pthread_t *pid;
        pthread_create(pid, NULL, flush_log_thread, NULL);
    }
    m_log_buf_size = log_buf_size;
    m_spine_lines = split_lines;
    m_buf = new char[m_log_buf_size];

    memset(m_buf, '\0', m_log_buf_size);
    //cout << "日志缓冲区大小：" << strlen(m_buf) << endl;

    time_t t = time(NULL);

    struct tm *sys_time = localtime(&t);
    struct tm my_tm = *sys_time; // 解引用
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};
    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }

    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }
    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");

    if (m_fp == nullptr)
    {
        return false;
    }
    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm1 = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    // 写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();

    m_count++;

    if (m_count % m_spine_lines == 0 || m_today != my_tm1.tm_mday)
    {
        char new_log[256] = {0};
        char tail[20] = {0};
        fflush(m_fp);
        fclose(m_fp);
        // 格式化日志名中的时间部分
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm1.tm_year + 1900, my_tm1.tm_mon + 1, my_tm1.tm_mday);
        if (m_today != my_tm1.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm1.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_spine_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    va_list valist;
    va_start(valist, format);

    string log_str; // 日志内容

    m_mutex.lock();
    // 写入的具体时间内容格式

    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm1.tm_year + 1900, my_tm1.tm_mon + 1, my_tm1.tm_mday,
                     my_tm1.tm_hour, my_tm1.tm_min, my_tm1.tm_sec, now.tv_usec, s);

    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valist);

    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }
    va_end(valist);
}
void Log::flush(void)
{
    m_mutex.lock();
    // 强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}