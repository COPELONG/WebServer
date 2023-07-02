#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <iostream>
#include <fstream>
// #define connfdET //边缘触发非阻塞
#define connfdLT // 水平触发阻塞

// #define listenfdET //边缘触发非阻塞
#define listenfdLT // 水平触发阻塞

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/long666/Code/Webserver/root";
// 将表中的用户名和密码放入map
map<string, string> users;
locker m_lock;
int http_conn::m_users_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::mysql_result(con_pool *pool)
{

    MYSQL *mysql = NULL;
    conn_rall mysqlcon(&mysql, pool);

    users.clear();
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error : %s\n", mysql_error(mysql));
    }
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int set_unblock(int fd)
{
    int Old = fcntl(fd, F_GETFL);
    int New = Old | O_NONBLOCK;
    fcntl(fd, F_SETFL, New);
    return Old;
}
// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool oneshot)
{

    epoll_event event;
    event.data.fd = fd;
#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef connfdET
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
#endif

    // #ifdef listenfdLT
    //     event.events = EPOLLIN | EPOLLRDHUP;
    // #endif

    // #ifdef listenfdET
    //     event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    // #endif

    if (oneshot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_unblock(fd);
}
// 从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, bool oneshot)
{

    epoll_event event;
    event.data.fd = fd;
#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLRDHUP;
#endif
    if (oneshot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
// 读取读缓冲区全部事件
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int byte_read = 0;

#ifdef connfdLT
    byte_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += byte_read;
    if (byte_read <= 0)
    {
        return false;
    }

    return true;
#endif

#ifdef connfdET
    while (true)
    {
        byte_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (byte_read <= 0)
        {
            return false;
        }
        m_read_idx += byte_read;
        return true;
    }

#endif
}
// 关闭连接
void http_conn::close_conn(bool Close)
{
    if (Close && m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_users_count--;
        m_sockfd = -1;
    }
}

// 初始化新接受的连接
// check_state默认为分析请求首行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_check_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_file_name, '\0', FILENAME_LEN);
}
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, m_sockfd, true);
    m_users_count++;
    init();
}
// char *http_conn::get_line()
// {
//     return m_read_buf + m_start_line;
// }
http_conn::LINE_STATUS http_conn::parse_line()
{

    char temp;
    // cout << "m_check_idx:" << m_check_idx << endl;
    // cout << "m_read_idx:" << m_read_idx << endl;
    for (; m_check_idx < m_read_idx; ++m_check_idx)
    {
        temp = m_read_buf[m_check_idx];
        if (temp == '\r')
        {
            if (m_check_idx + 1 == m_read_idx)
            {
                return LINE_OPEN; // 数据不完整，需要继续接受
            }
            else if (m_read_buf[m_check_idx + 1] == '\n')
            {
                m_read_buf[m_check_idx++] = '\0';
                m_read_buf[m_check_idx++] = '\0';
                // if (m_read_buf[m_check_idx - 1] == '\0' && m_read_buf[m_check_idx - 2] == '\0')
                // {
                //     cout << "parse_line()修改成功！！！！！！！！！" << endl;
                //     cout << "更新的m_check_idx:" << m_check_idx << endl;
                // }
                // else
                // {
                //     cout << "parse_line()末尾修改失败！！！！！！！！！！" << endl;
                // }
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_check_idx > 1 && m_read_buf[m_check_idx - 1] == '\r')
            {
                m_read_buf[m_check_idx - 1] = '\0';
                m_read_buf[m_check_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}
// 所以text可以直接取出完整的行进行解析

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{

    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        cout << "m_url出现错误246" << endl;
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        cout << "出现错误268" << endl;
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");

    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        cout << "出现错误275" << endl;
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t"); //  HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        cout << "m_version:" << m_version << endl;
        cout << "出现错误284" << endl;
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        cout << "出现错误301" << endl;
        return BAD_REQUEST;
    }
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 首先要判断是空行还是请求头部
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // std::cout << "unknow header" << endl;
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    // 如果没有符合头部的字符串，说明此行不符合规则，退出函数。
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 比较字节数，不能比读取的字节还大。
    if (m_read_idx >= m_check_idx + m_content_length)
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {

        text = m_read_buf + m_start_line;
        cout << "每一行的数据text是:" << text << endl;
        m_start_line = m_check_idx; // 及时更新行坐标。以便于下次循环获取一行数据。
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                cout << "parse_request_line(text)：出现错误" << endl;
                return BAD_REQUEST;
            }
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                cout << "do_request()进行中111:" << endl;
                return do_request();
            }
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                cout << "do_request()进行中222:" << endl;
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        default:
            INTERNAL_ERROR;
        }
    }
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        // 内部错误，500
    case INTERNAL_ERROR:
    {
        // 状态行
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    // 报文语法有误，404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
        // 资源没有访问权限，403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }

    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        // 如果请求的资源存在
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;

            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        { // 返回一个空界面
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) // 请求不完整，需要继续读取请求报文数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, true);
        return;
    }
    // 已经对写缓冲区和文件写入数据完成
    bool write_ret = process_write(read_ret);

    if (!write_ret)
    {
        close_conn(m_sockfd);
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, true);
}
// 对输出端进行写操作
bool http_conn::write()
{
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, true);
        init();
        return false;
    }
    int temp = 0;

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, true);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        if (bytes_to_send <= 0)
        {
            unmap();
            // 长连接重置http类实例，注册读事件，不关闭连接，
            modfd(m_epollfd, m_sockfd, EPOLLIN, true);
            // 短连接直接关闭连接
            if (m_linger)
            {

                init();
                return true;
            }
            return false;
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);

    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list);

    if (len > WRITE_BUFFER_SIZE - m_write_idx - 1)
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len; // 写缓冲区中已经写入的字符串
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();

    return true;
}
// 添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    // add_content_length(content_len);
    // add_linger();
    // add_blank_line();
    if (add_content_length(content_len) && add_linger() && add_blank_line())
    {
        return true;
    }
    return false;
}
// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// 添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
// 添加文本content{}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
// 调用do_request完成请求资源映射
http_conn::HTTP_CODE http_conn::do_request()
{

    strcpy(m_file_name, doc_root);

    int len = strlen(doc_root);
    // 找到m_url中/的位置
    const char *p = strchr(m_url, '/');
    cout << "m_url:" << m_url << endl;
    // 实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        // 同步线程登录校验
        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
        // CGI多进程登录校验
    }

    if (*(p + 1) == '0')
    {

        char *m_real_url = (char *)malloc(sizeof(char) * 200);

        strcpy(m_real_url, "/register.html");

        strncpy(m_file_name + len, m_real_url, strlen(m_real_url));

        free(m_real_url);
    }
    else if (*(p + 1) == '1')
    {

        char *m_real_url = (char *)malloc(sizeof(char) * 200);

        strcpy(m_real_url, "/log.html");

        strncpy(m_file_name + len, m_real_url, strlen(m_real_url));

        free(m_real_url);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_file_name + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_file_name + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_file_name + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
    {
        strncpy(m_file_name + len, m_url, FILENAME_LEN - len - 1);
    }

    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_file_name, &m_file_stat) < 0)
    {
        cout << "NO_RESOURCE" << endl;
        return NO_RESOURCE;
    }
    else
    {
        cout << "RESOURCE请求存在" << endl;
    }
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        cout << "FORBIDDEN_REQUEST" << endl;
        return FORBIDDEN_REQUEST;
    }
    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
    {
        cout << "BAD_REQUEST" << endl;
        return BAD_REQUEST;
    }
    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_file_name, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    cout << "m_file_address映射成功" << endl;
    close(fd);
    cout << "do_request即将返回" << endl;
    return FILE_REQUEST;
}
void http_conn::unmap()
{

    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
