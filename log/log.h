#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
using namespace std;
class Log
{
public:
    static Log *get_instance()
    {
        static Log m_log;
        return &m_log;
    }
    // 异步写日志公有方法，调用私有方法async_write_log
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }
    // 将输出内容按照标准格式整理
    void write_log(int level, const char *format, ...);
    // //强制刷新缓冲区
    void flush(void);
    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

private:
    Log();
    ~Log();
    // 异步写日志方法
    void *async_write_log()
    {
        string one_log;
        while (m_log_queue->pop(one_log))
        {
            m_mutex.lock();

            fputs(one_log.c_str(), m_fp);

            m_mutex.unlock();
        }
    }

private:
    char dir_name[200];
    char log_name[200];
    int m_spine_lines;  // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小
    long long m_count;  // 日志行数记录
    int m_today;
    FILE *m_fp;                       // 打开log的文件指针
    char *m_buf;                      // 要输出的内容
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否同步标志位
    locker m_mutex;
};

#define LOG_DEBUG(format, ...) \
    Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) \
    Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) \
    Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) \
    Log::get_instance()->write_log(3, format, ##__VA_ARGS__)
#endif