#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

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
#include "../pool/connection_pool.h"

// http连接类
class http_connection {
	public:
		// 请求文件的文件名大小
		static const int FILE_NAME_SIZE = 200;
		// 读写缓冲区大小
		static const int READ_BUFFER_SIZE = 2048;
		static const int WRITE_BUFFER_SIZE = 1024;
		// 请求方法：GET、POST（本项目只用到了这两种）
		enum class REQUEST_METHOD { GET, POST };
		// http状态码：请求尚未完整、获得了完整请求、存在语法错误、服务器内部错误
		// 请求资源不存在、请求资源禁止访问、请求资源可以访问、关闭http连接
		enum class HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, INTERNAL_ERROR,
			NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, CLOSED_CONNECTION };
		// 主状态机状态：检查请求行、检查请求头、检查请求数据
		enum class CHECK_STATUS { CHECK_REQUEST_LINE, CHECK_HEADER, CHECK_CONTENT };
		// 从状态机状态：成功解析完一行、存在语法错误、尚未成功解析完一行
		enum class LINE_STATUS { LINE_OK, LINE_BAD, LINE_OPEN };

	public:
		static int _epollfd; // epoll对象的文件描述符
		static int _user_count; // 连接的客户端的数量
		MYSQL *_mysql; // 数据库连接

	private:
		int _sockfd; // 与客户端连接的文件描述符
		sockaddr_in _address; // 客户端的socket地址

		char _read_buf[READ_BUFFER_SIZE]; // 读缓冲区
		int _read_idx; // 读缓冲区中最后一个字节数据的下一个位置
		int _checked_idx; // 读缓冲区中当前正在读取的数据的位置
		int _start_line; // 读缓冲区中已解析的字符个数

		char _write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
		int _write_idx; // 写缓冲区中已写入的字符个数

		CHECK_STATUS _check_status; // 主状态机的状态

		// 请求行中的字段
		REQUEST_METHOD _request_method; // 请求方法
		bool _cgi; // 请求方式是否为POST
		char *_url; // 请求资源的url
		char *_version; // http版本号

		// 请求头中的字段
		char *_host; // 服务器的域名
		int _content_length; // 记录请求数据的长度，若为POST方式，则该值大于0
		bool _linger; // 连接管理（长连接：keep-alive、短连接：close）

		char _real_file[FILE_NAME_SIZE]; // 请求资源的文件路径
		char *_file_address; // 请求资源的文件所映射到的内存地址
		struct stat _file_stat; // 记录所请求的资源文件的文件属性
		// iovec数组第一个元素指向写缓冲区，第二个指向资源文件所映射到的内存地址
		// 若请求的资源文件出错（不存在或无权访问），则只有第一个元素有效，并指向写缓冲区
		struct iovec _iv[2];
		int _iv_count; // 记录有效的iovec个数

		char *_user_info; // 当请求方式为POST时，存放POST携带的数据（用户名和密码）

		int _bytes_sent; // 已发送的字节数
		int _bytes_left; // 剩余待发送的字节数

	public:
		// 使用默认合成的构造函数和析构函数
		http_connection() = default;
		~http_connection() = default;

	public:
		// 初始化连接，设置客户端socket文件描述符和socket地址等信息?
		void init(int sockfd, const sockaddr_in &addr);

		// 关闭连接，将与客户端连接的文件描述符从epoll内核事件表中移除
		void close_connection(bool real_close = true);

		// 从数据库中检索出所有的用户数据，用于登录校验
		void init_mysql_result(connection_pool *conn_pool);

		// 由工作线程执行的任务处理函数，完成对报文的解析和响应
		void process();

		// 从套接字中读取客户数据，有LT和ET两种模式
		// LT模式下，每次调用可读一部分数据，无需一次性收取所有数据
		// ET模式下，每次调用必须通过循环来将所有数据一次性接收干净
		bool read_once();

		// 将响应报文写入并发送给客户端浏览器
		bool write();

		// 获取客户端的socket地址?
		sockaddr_in* get_address() { return &_address; }

	private:
		// 初始化新接受的连接?
		void init();

		// 利用主从状态机解析请求报文（请求行、请求头、请求数据）
		HTTP_CODE process_read();
		// 从状态机：用于解析一行数据（将行尾结束符由\r\n替换为\0\0）
		LINE_STATUS parse_line();
		// 主状态机：用于解析请求行数据（获取请求方法、url、http版本号）
		HTTP_CODE parse_request_line(char *text);
		// 主状态机：用于解析请求头（获取host、connection、content-length）或空行
		HTTP_CODE parse_headers(char *text);
		// 主状态机：用于解析请求数据（也叫请求主体）
		HTTP_CODE parse_content(char *text);

		// 执行客户端请求，根据不同的请求执行对应的操作
		HTTP_CODE exec_request();

		// 根据http状态码向写缓冲区中写入响应报文
		bool process_write(HTTP_CODE ret);
		// 生成响应报文的状态行（http版本号、状态码、状态消息）
		bool add_status_line(int status, const char *title) { return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); }
		// 生成响应报文的消息头（content-length响应正文长度、connection连接管理）
		bool add_headers(int content_length) { return add_response("Content-Length:%d\r\nConnection:%s\r\n", content_length, (_linger ? "keep-alive" : "close")); }
		// 生成响应报文的空行
		bool add_blank_line() { return add_response("%s", "\r\n"); }
		// 生成响应报文的响应正文
		bool add_content(const char *content) { return add_response("%s", content); }
		// 利用可变参将响应信息存入写缓冲区
		bool add_response(const char *format, ...);

		/* bool add_headers(int content_length) { return add_content_length(content_length) && add_linger() && add_blank_line(); } */
			/* add_content_length(content_length) && add_linger() && add_blank_line(); } */
		// 生成响应报文中消息头的content-length响应正文长度字段
		/* bool add_content_length(int content_length) { return add_response("Content-Length:%d\r\n", content_length); } */

		/* bool add_content_type() { return add_response("Content-Type:%s\r\n", "text/html"); } */
		/* bool add_linger() { return add_response("Connection:%s\r\n", (_linger == true) ? "keep-alive" : "close"); } */
};

#endif
