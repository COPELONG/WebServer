#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    // 解析返回的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    // 数据库姓名和密码存储在map中
    void mysql_result(con_pool *pool);
    // 初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr);
    // 关闭http连接
    void close_conn(bool close = true);

    // 请求队列中任务执行函数
    void process();

    // 从浏览器端读取全部发送的数据
    bool read_once();

    // 响应报文写入函数
    bool write();

    sockaddr_in *get_address()
    {
        return &m_address;
    }

public:
    http_conn() {}
    ~http_conn()
    {
    }

private:
    void init();
    // m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
    // 此时从状态机已提前将一行的末尾字符\r\n变为\0\0，
    // 所以text可以直接取出完整的行进行解析
    //char *get_line();
    void unmap();
    LINE_STATUS parse_line();                 // 解析这一行是属于请求报文的哪一部分，然后传入对应解析函数中
    HTTP_CODE process_read();                 // 读取请求报文
    HTTP_CODE parse_request_line(char *text); // 解析报文的请求首行
    HTTP_CODE parse_headers(char *text);      // 解析报文的请求头
    HTTP_CODE parse_content(char *text);      // 解析报文的请求体

    HTTP_CODE do_request();            // 生成响应报文
    bool process_write(HTTP_CODE ret); // 向写缓冲区写入响应报文

    // do_request调用以下函数，生成响应报文对应的每个部分。
    bool add_response(const char *format, ...);

    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_users_count;
    MYSQL *mysql;

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE]; // 存储读取的请求报文数据
    int m_read_idx;                    // 缓冲区中当前的字符个数
    int m_check_idx;                   // 缓冲区中当前读取的位置
    int m_start_line;                  // 读取行，已经解析的字符个数
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx; // 写缓冲区当前的字符个数
    METHOD m_method;
    CHECK_STATE m_check_state;
    char m_file_name[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address; // 读取服务器上的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;             // 是否启用的POST
    char *m_string;      // 存储请求头数据
    int bytes_to_send;   // 剩余发送字节数
    int bytes_have_send; // 已发送字节数
};

#endif
