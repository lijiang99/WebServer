#include <map>
#include <mysql/mysql.h>
#include <fstream>
#include <mutex>
#include "http_connection.h"
#include "../log/log.h"

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

// http响应状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 请求资源所在的根目录
const char *doc_root = "/home/bd7xzz/Desktop/WebServer/root";

// 该map用于记录数据库user数据表中已经存在的用户信息（用户名和密码）
std::map<std::string, std::string> users;

// 互斥锁
std::mutex mtx;

// 将文件描述符设置为非阻塞式
int set_nonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向epoll的内核事件表中注册读事件，且ET模式下，需针对客户端连接的文件描述符开启EPOLLONESHOT
// 因为即使是ET模式，一个socket上的某个事件还是可能会多次触发，在并发环境会出现问题（两个线程同时操作一个socket）
// 所以必须确保一个socket连接在任意时刻都只被一个线程处理，而EPOLLONESHOT可确保最多触发一次可读、可写或异常事件
void add_fd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking(fd);
}

// 重置EPOLLONESHOT事件，因为add_fd中为sockfd注册了EPOLLONESHOT事件
// 而一旦该sockfd被某个线程处理完毕，该线程就该立即重置该sockfd上的EPOLLONESHOT事件
// 以确保该sockfd下一次可读时，其EPOLLIN事件能被触发，从而让工作线程可再次处理该sockfd
void reset_fd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 从epoll内核事件表中删除代表事件的文件描述符
void remove_fd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 初始化类的静态成员变量（epoll对象的文件描述符、连接的客户端的数量）
int http_connection::_epollfd = -1;
int http_connection::_user_count = 0;

// 初始化连接，设置客户端socket文件描述符和socket地址等信息?
void http_connection::init(int sockfd, const sockaddr_in &addr) {
	// 向epoll内核事件表中注册客户端连接的socket，并启用EPOLLONESHOT?
    add_fd(_epollfd, sockfd, true); ++_user_count;
	// 设置客户端信息（连接的socket文件描述符和socket地址）?
    _sockfd = sockfd; _address = addr;
	init();
}

// 初始化新接受的连接?
void http_connection::init() {
	// 初始化数据库连接
	_mysql = nullptr;

	// 初始化读缓冲区和索引位置
    bzero(_read_buf, READ_BUFFER_SIZE);
    _read_idx = 0; _checked_idx = 0; _start_line = 0;

	// 初始化写缓冲区和索引位置
    bzero(_write_buf, WRITE_BUFFER_SIZE);
    _write_idx = 0;

	// 初始化主状态机状态，请求行需要第一个检查
    _check_status = CHECK_STATUS::CHECK_REQUEST_LINE;

	// 初始化请求行中的字段（请求方法--默认为GET、url、http版本号）
    _request_method = REQUEST_METHOD::GET;
    _cgi = false; _url = nullptr; _version = nullptr;

	// 初始化请求头中的字段（服务器域名、请求数据长度、连接管理--默认为短连接：close）
    _host = nullptr; _content_length = 0; _linger = false;

	// 初始化所请求资源的文件路径
    bzero(_real_file, FILE_NAME_SIZE);
	_file_address = nullptr;

	// 初始化字节数（已发送/待发生）
    _bytes_sent = 0; _bytes_left = 0;
}

// 关闭连接，并递减对应的连接客户端计数器
void http_connection::close_connection(bool real_close) {
    if (real_close && (_sockfd != -1)) {
		// 从epoll内核事件表中移除对应的文件描述符并将该文件描述符设置为-1
        remove_fd(_epollfd, _sockfd);
        _sockfd = -1; --_user_count;
    }
}

// 将数据库中已有的所有用户数据检索出来并存入users中，用于登录校验
void http_connection::init_mysql_result(connection_pool *conn_pool) {
    // 从连接池中取一个连接
    MYSQL *mysql = nullptr;
    sql_connection mysql_conn(&mysql, conn_pool);

    // 在user数据表中检索所有用户的数据
    if (mysql_query(mysql, "SELECT username, passwd FROM user"))
		LOG_ERROR("mysql select error: ", mysql_error(mysql));

    // 获取检索的结果集，并通过循环每次从结果集中取出下一条用户数据存入map中
    MYSQL_RES *result = mysql_store_result(mysql);
    while (MYSQL_ROW row = mysql_fetch_row(result)) users[row[0]] = row[1];
}

// 由工作线程执行的任务处理函数，完成对报文的解析和响应
void http_connection::process() {
	// 解析请求报文，若返回结果为HTTP_CODE::NO_REQUEST
	// 表示尚未解析到完整请求，则需继续接收请求数据以供解析
    HTTP_CODE read_ret = process_read();
    if (read_ret == HTTP_CODE::NO_REQUEST) { reset_fd(_epollfd, _sockfd, EPOLLIN); return; }
	// 若解析到了完整的请求，则向写缓冲区写入数据完成对请求报文的响应?
	if (!process_write(read_ret)) close_connection();
	// 注册EPOLLOUT事件，使主线程可检测写事件，以通过write将响应报文发送给客户端（浏览器）
    reset_fd(_epollfd, _sockfd, EPOLLOUT);
}

// 当有读事件发生，则从套接字中读取客户数据，有LT和ET两种模式
// LT模式下，每次调用可读一部分数据，无需一次性收取所有数据
// ET模式下，每次调用必须通过循环来将所有数据一次性接收干净
bool http_connection::read_once() {
    if (_read_idx >= READ_BUFFER_SIZE) return false;

    int bytes_read = 0;

#ifdef connfdLT
    bytes_read = recv(_sockfd, _read_buf + _read_idx, READ_BUFFER_SIZE - _read_idx, 0);
    if (bytes_read <= 0) return false;
    _read_idx += bytes_read;
    return true;
#endif

#ifdef connfdET
    while (true) {
		bytes_read = recv(_sockfd, _read_buf + _read_idx, READ_BUFFER_SIZE - _read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        else if (bytes_read == 0) return false;
        _read_idx += bytes_read;
    }
    return true;
#endif
}

// 当主线程检测到写事件，会调用该函数将响应报文发送给客户端浏览器
bool http_connection::write() {
	// 若待发送的数据长度为0，则表示响应报文为空，一般不会出现该情况
    if (_bytes_left == 0) { reset_fd(_epollfd, _sockfd, EPOLLIN); init(); return true; }

    int tmp = 0;
    while (true) {
		// 将响应报文的状态行、消息头、空行以及响应正文发送给客户端浏览器
        tmp = writev(_sockfd, _iv, _iv_count);

        if (tmp < 0) {
			// 若缓冲区已经满了，则重新注册写事件
            if (errno == EAGAIN) { reset_fd(_epollfd, _sockfd, EPOLLOUT); return true; }
			// 否则表示发送失败，且不是缓冲区问题，因此解除文件到内存的映射
			if (_file_address) { munmap(_file_address, _file_stat.st_size); _file_address = nullptr; }
            return false;
        }

		// 若正常发送，则tmp为发送的字节数，需更新已发送/待发送字节数
        _bytes_sent += tmp; _bytes_left -= tmp;

		// 若第一个iovec的头部信息数据已发送完毕，则发送第二个iovec中的数据
        if (_bytes_sent >= _iv[0].iov_len) {
            _iv[0].iov_len = 0;
            _iv[1].iov_base = _file_address + (_bytes_sent - _write_idx);
            _iv[1].iov_len = _bytes_left;
        }
		// 若第一个iovec的头部数据尚未发送完，则继续发送
        else {
            _iv[0].iov_base = _write_buf + _bytes_sent;
            _iv[0].iov_len = _iv[0].iov_len - _bytes_sent;
        }

		// 数据已经全部发送完毕
        if (_bytes_left <= 0) {
			// 解除文件到内存的映射，并释放相关资源
			if (_file_address) { munmap(_file_address, _file_stat.st_size); _file_address = nullptr; }
            reset_fd(_epollfd, _sockfd, EPOLLIN);
			// 若为长连接，则重置http连接对象的信息
            if (_linger) { init(); return true; }
			// 否则为短连接，则需要断开连接
            else return false;
        }
    }
}

// 根据主从状态机状态，通过循环来不停地解析请求报文中的数据
http_connection::HTTP_CODE http_connection::process_read() {
	// 初始化从状态机（成功解析完一行）和报文解析结果（请求尚未完整）
    LINE_STATUS line_status = LINE_STATUS::LINE_OK;
    HTTP_CODE ret = HTTP_CODE::NO_REQUEST;
    char *text = nullptr;

	// 循环条件1: 正在检测请求数据内容（POST才有），由于GET请求中的报文每行都以\r\n为结束符
	// 而POST请求数据无任何结束符，所有不能通过从状态机，而应该通过主状态机判断
	// 若请求数据全部解析完毕，则表明整个报文也解析完毕，但主状态仍为CHECK_STATUS::CHECK_CONTENT
	// 所以需要从状态机辅助判断，当全部解析完毕，就将从状态设置为LINE_STATUS::LINE_OPEN以退出循环
	// 循环条件2: 正在检测请求行或请求头，且每次进入循环前，从状态机都已成功解析完一行
	// 即读取完了一行，且将结束符从\r\n替换为了\0\0
    while ((_check_status == CHECK_STATUS::CHECK_CONTENT && line_status == LINE_STATUS::LINE_OK)
			|| ((line_status = parse_line()) == LINE_STATUS::LINE_OK)) {
		// 获取将要解析的行的起始位置
        text = _read_buf + _start_line;
        _start_line = _checked_idx;
		// 从状态机的三种状态转移逻辑
        switch (_check_status) {
			// 表示正在解析请求行内容
			case CHECK_STATUS::CHECK_REQUEST_LINE:
				{
					ret = parse_request_line(text);
					if (ret == HTTP_CODE::BAD_REQUEST) return HTTP_CODE::BAD_REQUEST;
					break;
				}
			// 表示正在解析请求头内容
			case CHECK_STATUS::CHECK_HEADER:
				{
					ret = parse_headers(text);
					if (ret == HTTP_CODE::BAD_REQUEST) return HTTP_CODE::BAD_REQUEST;
					// 由于GET请求无请求数据，所以解析完请求头后可直接执行响应函数
					else if (ret == HTTP_CODE::GET_REQUEST) return exec_request();
					break;
				}
			// 表示正在解析请求数据内容（请求方法为POST时）
			case CHECK_STATUS::CHECK_CONTENT:
				{
					ret = parse_content(text);
					// 若完整解析了POST的请求数据，则表示报文已全部解析完毕，可执行响应函数
					if (ret == HTTP_CODE::GET_REQUEST) return exec_request();
					// 将从状态设置为LINE_STATUS::LINE_OPEN，以免再次进入循环
					line_status = LINE_STATUS::LINE_OPEN;
					break;
				}
			// 若三种状态都不符，则表示出现服务器内部错误，一般不会触发
			default:
				return HTTP_CODE::INTERNAL_ERROR;
		}
	}
	// 表示整个http请求尚未完整，需要继续接收请求数据以供解析
    return HTTP_CODE::NO_REQUEST;
}

// 从状态机：用于解析一行数据，返回LINE_STATUS值，表示解析结果
http_connection::LINE_STATUS http_connection::parse_line() {
    for (char tmp; _checked_idx < _read_idx; ++_checked_idx) {
        tmp = _read_buf[_checked_idx];
		// 若当前为\r字符，则可能会读取到完整的行
        if (tmp == '\r') {
			// 若下一个字符到达了读缓冲区的末尾，则表示接收尚未完整，需继续接收
            if ((_checked_idx + 1) == _read_idx) return LINE_STATUS::LINE_OPEN;
			// 若下一个字符为\n，则表明接收到了完整的行，并将\r\n改为\0\0表示解析成功
            else if (_read_buf[_checked_idx + 1] == '\n') {
                _read_buf[_checked_idx++] = '\0';
                _read_buf[_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
			// 若都不符合，则返回语法错误
            return LINE_STATUS::LINE_BAD;
        }
		// 若当前字符为\n，也可能会读取到完整的行
		// 即上次读到\r就到了读缓冲区的末尾，没有接收完整
        else if (tmp == '\n') {
			// 前一个字符为\r，则表示接收完整
            if (_checked_idx > 1 && _read_buf[_checked_idx - 1] == '\r') {
                _read_buf[_checked_idx - 1] = '\0';
                _read_buf[_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
            return LINE_STATUS::LINE_BAD;
        }
    }
	// 尚未找到\r\n，则需要继续接收
    return LINE_STATUS::LINE_OPEN;
}


// 主状态机：用于解析请求行数据，以获取请求方法、url、http版本号
http_connection::HTTP_CODE http_connection::parse_request_line(char *text) {
	// 请求行内容中的字段以空格或\t分隔，找到第一个空格或\t，则表示找到了url
    _url = strpbrk(text, " \t");
	// 若无空格或\t，则请求报文有语法错误
    if (!_url) return HTTP_CODE::BAD_REQUEST;

	// 将该位置改为\0，用于将前面的字段取出（请求方法）
    *_url++ = '\0';
	// 解析请求方法字段，本项目只用到了GET和POST
    char *method = text;
    if (strcasecmp(method, "GET") == 0) _request_method = REQUEST_METHOD::GET;
    else if (strcasecmp(method, "POST") == 0) { _request_method = REQUEST_METHOD::POST; _cgi = 1; }
    else return HTTP_CODE::BAD_REQUEST;

	// 向后移动，跳过空格或\t，以找到http版本号位置
    _url += strspn(_url, " \t");
	// 解析版本号字段（仅支持http1.1），与上面的操作类似
    _version = strpbrk(_url, " \t");
    if (!_version) return HTTP_CODE::BAD_REQUEST;
    *_version++ = '\0';
    _version += strspn(_version, " \t");
    if (strcasecmp(_version, "HTTP/1.1") != 0) return HTTP_CODE::BAD_REQUEST;

	// 处理url中携带http(s)://的情况
    if (strncasecmp(_url, "http://", 7) == 0) { _url += 7; _url = strchr(_url, '/'); }
    if (strncasecmp(_url, "https://", 8) == 0) { _url += 8; _url = strchr(_url, '/'); }
	// url中无上述两种符号，直接是单独的/或//后接访问资源，则请求报文有语法错误
    if (!_url || _url[0] != '/') return HTTP_CODE::BAD_REQUEST;

    // 当url为/时，默认显示校验界面
    if (strlen(_url) == 1) strcat(_url, "judge.html");
    _check_status = CHECK_STATUS::CHECK_HEADER;
    return HTTP_CODE::NO_REQUEST;
}

// 主状态机：用于解析请求头或空行
// 若为请求头，则解析host服务器域名字段、connection连接管理字段、content-length请求数据长度字段
// 若为空行，需要判断content-length字段是否为0，为0则是GET，且无需再解析，否则为POST
http_connection::HTTP_CODE http_connection::parse_headers(char *text) {
	// 判断是空行还是请求头，由于每次调用前，从状态机都已完成了对一行的解析
	// 因此若为空行\r\n，则必定已经被替换为了\0\0
    if (text[0] == '\0') {
		// 请求方法字段为POST才会有请求数据，此时_content_length已在解析请求头的过程中完成设置
        if (_content_length != 0) {
            _check_status = CHECK_STATUS::CHECK_CONTENT;
            return HTTP_CODE::NO_REQUEST;
        }
		// 否则为GET请求，表明请求报文已经全部解析完毕
        return HTTP_CODE::GET_REQUEST;
    }
	// 若不为空行，则解析请求头
	// 解析请求头中的连接管理字段（keep-alive长连接、close短连接）
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11; text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) _linger = true;
    }
	// 解析请求头中的请求数据长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15; text += strspn(text, " \t");
        _content_length = atol(text);
    }
	// 解析请求头中的服务器域名字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5; text += strspn(text, " \t");
        _host = text;
    }
    else {
        LOG_INFO("oop! unknow header: ", text);
        log::get_instance()->flush();
    }
    return HTTP_CODE::NO_REQUEST;
}

// 主状态机：用于解析请求数据（也叫请求主体），仅当请求方法为POST时才会调用
http_connection::HTTP_CODE http_connection::parse_content(char *text) {
	// 判断读缓冲区是否已经读取了完整的请求数据
    if (_read_idx >= (_content_length + _checked_idx)) {
        text[_content_length] = '\0';
		// 对于POST请求，只能处理其携带用户名和密码的情况
        _user_info = text;
        return HTTP_CODE::GET_REQUEST;
    }
    return HTTP_CODE::NO_REQUEST;
}

// 执行客户端的请求，根据不同的请求执行对应的操作
// 若为POST，则执行登录/注册校验，若为GET，则将对应的资源文件（html）映射到内存中
http_connection::HTTP_CODE http_connection::exec_request() {
	// 将_real_file设置为网站根目录
    strcpy(_real_file, doc_root);
    int len = strlen(doc_root);

	// 找到url中/的位置
    const char *p = strrchr(_url, '/');

    // 若_cgi为true，则表示是POST请求，需要进行登录校验或注册校验
    if (_cgi == true && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录校验还是注册校验
        char flag = _url[1];
        char *url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(url_real, "/");
        strcat(url_real, _url + 2);
        strncpy(_real_file + len, url_real, FILE_NAME_SIZE - len - 1);
        free(url_real);

        // 将用户名和密码提取出来，POST请求通过&字符来连接字段
        char name[100], password[100];
        int i = 5, j = 0;
        while (_user_info[i] != '&') { name[i-5] = _user_info[i]; ++i; }
        name[i - 5] = '\0'; i += 10;
        while (_user_info[i] != '\0') password[j++] = _user_info[i++];
        password[j] = '\0';

        // 同步线程注册校验
        if (*(p + 1) == '3') {
            // 若为注册，先检测数据库中用户名是否已存在，若尚为存在，则新增数据
            char *sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) {
				std::unique_lock<std::mutex> lock(mtx);
                int res = mysql_query(_mysql, sql_insert);
                users.insert(std::pair<std::string, std::string>(name, password));
                lock.unlock();

                if (!res) strcpy(_url, "/log.html");
                else strcpy(_url, "/registerError.html");
            }
            else strcpy(_url, "/registerError.html");
        }
		// 登录校验，若客户端输入的用户名和密码在全局的users中可以查到，则登录成功
        else if (*(p + 1) == '2') {
            if (users.find(name) != users.end() && users[name] == password) strcpy(_url, "/welcome.html");
            else strcpy(_url, "/logError.html");
        }
    }

	// 根据不同的请求资源类型，跳转到不同的资源页面
	char *tmp_url = (char*)malloc(sizeof(char) * 200);
	bzero(tmp_url, sizeof(char)*200);
	switch(*(p + 1)) {
		case '0':
			strcpy(tmp_url, "/register.html"); break;
		case '1':
			strcpy(tmp_url, "/log.html"); break;
		case '5':
			strcpy(tmp_url, "/picture.html"); break;
		case '6':
			strcpy(tmp_url, "/video.html"); break;
		case '7':
			strcpy(tmp_url, "/fans.html"); break;
		default: break;
	}
	char *real_url = tmp_url[0] ? tmp_url : _url;
	int url_size = tmp_url[0] ? strlen(real_url) : FILE_NAME_SIZE - len - 1;
	strncpy(_real_file + len, real_url, url_size);  
	free(tmp_url); tmp_url = nullptr; real_url = nullptr;

	// 通过stat获取资源文件的文件属性，成功则将属性信息存入_file_stat，失败则返回资源不存在
    if (stat(_real_file, &_file_stat) < 0) return HTTP_CODE::NO_RESOURCE;
	// 判断文件权限是否可读，不可读返回禁止访问资源
    if (!(_file_stat.st_mode & S_IROTH)) return HTTP_CODE::FORBIDDEN_REQUEST;
	// 判断文件类型是否为目录，若为目录，则表明请求报文有错误
    if (S_ISDIR(_file_stat.st_mode)) return HTTP_CODE::BAD_REQUEST;
	// 以只读模式打开文件
    int fd = open(_real_file, O_RDONLY);
	// 将文件映射到内存，提高读取速度，并返回文件映射到的内存地址
    _file_address = (char*)mmap(0, _file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
	// 返回请求资源存在且允许访问
    return HTTP_CODE::FILE_REQUEST;
}

// 根据http状态码向写缓冲区中写入响应报文
bool http_connection::process_write(HTTP_CODE ret) {
    switch (ret) {
		// 服务器内部出现错误
		case HTTP_CODE::INTERNAL_ERROR:
			{
				add_status_line(500, error_500_title);
				add_headers(strlen(error_500_form));
				add_blank_line();
				if (!add_content(error_500_form)) return false;
				break;
			}
		// 请求报文存在语法错误
		case HTTP_CODE::BAD_REQUEST:
			{
				add_status_line(404, error_404_title);
				add_headers(strlen(error_404_form));
				add_blank_line();
				if (!add_content(error_404_form)) return false;
				break;
			}
		// 无权访问请求资源
		case HTTP_CODE::FORBIDDEN_REQUEST:
			{
				add_status_line(403, error_403_title);
				add_headers(strlen(error_403_form));
				add_blank_line();
				if (!add_content(error_403_form)) return false;
				break;
			}
		// 资源文件存在且有权访问
		case HTTP_CODE::FILE_REQUEST:
			{
				add_status_line(200, ok_200_title);
				if (_file_stat.st_size != 0) {
					add_headers(_file_stat.st_size);
					add_blank_line();
					// 将iovec中第一个元素设置为指向写缓冲区的指针
					_iv[0].iov_base = _write_buf;
					_iv[0].iov_len = _write_idx;
					// 将iovec中第二个元素设置为资源文件所映射到的内存地址
					_iv[1].iov_base = _file_address;
					_iv[1].iov_len = _file_stat.st_size;
					// 设置iovec的有效元素个数为2
					_iv_count = 2;
					// 设置待发送的字节数为响应报文的状态行、消息头、空行
					// 以及响应正文（资源文件）的总和
					_bytes_left = _write_idx + _file_stat.st_size;
					return true;
				}
				// 若资源文件的大小为0，则返回空白的html文件
				else {
					const char *ok_string = "<html><body></body></html>";
					add_headers(strlen(ok_string));
					add_blank_line();
					if (!add_content(ok_string)) return false;
				}
			}
		default: return false;
	}
	// 除了HTTP_CODE::FILE_REQUEST，其余状态都不会设置资源文件到内存的映射
	// 所以无需设置iovec的第二个元素，只需将iovec第一个元素设置为指向写缓冲区即可
    _iv[0].iov_base = _write_buf;
    _iv[0].iov_len = _write_idx;
	// 设置iovec的有效元素个数为1
    _iv_count = 1;
	// 设置待发送的字节数为响应报文的状态行、消息头、空行的总和（无响应正文，即资源文件）
    _bytes_left = _write_idx;
    return true;
}

// 利用可变参将响应信息存入写缓冲区，每次存入均需更新写缓冲区中的位置
bool http_connection::add_response(const char *format, ...) {
	// 若已存入的内容超出了写缓冲区的大小，则报错
    if (_write_idx >= WRITE_BUFFER_SIZE) return false;
	// 定义可变参列表，并将其初始化为传入的format
    va_list arg_list;
    va_start(arg_list, format);
	// 将format从可变参数列表存入写缓冲区，并返回存入数据的长度
    int len = vsnprintf(_write_buf + _write_idx, WRITE_BUFFER_SIZE - 1 - _write_idx, format, arg_list);
	// 若存入的数据长度超过写缓冲区的剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - _write_idx)) { va_end(arg_list); return false; }
	// 更新写缓冲区中的位置
    _write_idx += len;
	// 清空可变参列表
    va_end(arg_list);
    LOG_INFO("request: ", _write_buf);
    log::get_instance()->flush();
    return true;
}
