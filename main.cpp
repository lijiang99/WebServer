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
#include <iostream>

#include "pool/thread_pool.h"
#include "timer/timer.h"
#include "http/http_connection.h"
#include "log/log.h"
#include "pool/connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

/* #define SYNLOG  //同步写日志 */
#define ASYNLOG //异步写日志

/* #define listenfdLT //水平触发阻塞 */
#define listenfdET //边缘触发非阻塞

// 在http_conn.cpp中定义，改变连接属性
extern int add_fd(int epollfd, int fd, bool one_shot);
extern int set_nonblocking(int fd);

static int pipefd[2];
static int epollfd = 0;

// 使用时间堆来管理定时器
static timer_heap<util_timer> timer_manager;

//信号处理函数
void sig_handler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    /* assert(sigaction(sig, &sa, NULL) != -1); */
    sigaction(sig, &sa, NULL) != -1;
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler() {
    timer_manager.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，从epoll内核事件表中删除客户对应的sockfd，并将其关闭
void cb_func(client_data *user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
	// **********
	user_data->timer = nullptr;
	// **********
    http_connection::_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    log::get_instance()->flush();
}

void show_error(int connfd, const char *info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
#ifdef ASYNLOG
    log::get_instance()->init("./", 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    log::get_instance()->init("./", 800000, 0); //同步日志模型
#endif

    if (argc <= 1) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    connection_pool *connPool = connection_pool::get_instance();
    connPool->init("localhost", "root", 3306, "$Li&&990503", "web_server", 8);

    // 创建线程池
    thread_pool<void()> *pool = NULL;
    pool = new thread_pool<void()>(8,10000);
	assert(pool);

    http_connection *users = new http_connection[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->init_mysql_result(connPool);

	// 创建监听socket文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
	// 创建监听socket的TCP/IP协议族的IPV4地址
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
	// INADDR_ANY: 将套接字绑定到所有可用的接口
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
	// SO_REUSEADDR: 允许端口被重复使用
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	// 绑定IP地址和端口号
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
	// 创建监听队列存放客户连接（随机到达的异步事件），默认队列长度为5
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 创建epoll内核事件表，并用epollfd标识
    epollfd = epoll_create(5);
    assert(epollfd != -1);
	// 用于存放epoll事件表中就绪事件的events数组
    epoll_event events[MAX_EVENT_NUMBER];

	// 主线程将listenfd注册到epoll内核表中
	// 当监听到新的客户连接，listenfd就变为就绪事件
    add_fd(epollfd, listenfd, false);
    http_connection::_epollfd = epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    set_nonblocking(pipefd[1]);
    add_fd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

	// 用于保存客户端数据（IP地址、文件描述符、定时器）的数组
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while (!stop_server) {
		// 主线程调用epoll_wait等待就绪事件
		// 并将当前所有就绪事件复制到events数组中
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
		// 通过遍历events数组来处理已经就绪的事件
        for (int i = 0; i < number; i++) {
			// 就绪事件的socket文件描述符
            int sockfd = events[i].data.fd;

			// 当listenfd监听到新的客户连接，那么listenfd产生就绪事件
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
				// LT模式
#ifdef listenfdLT
				// accept返回新的文件描述符connfd用于收发数据
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0) {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_connection::_user_count >= MAX_FD) {
					// 客户数量已达到上线，向新客户发送服务器繁忙信息，并关闭当前connfd
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
				// 将connfd注册到epoll内核事件表中
                users[connfd].init(connfd, client_address);

                // 初始化client_data数据
                // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到时间堆中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->timeout_callback = cb_func;
				timer->expire = std::chrono::high_resolution_clock::now() + 3*std::chrono::seconds(TIMESLOT);
                users_timer[connfd].timer = timer;
                timer_manager.push_timer(timer);
#endif

				// ET模式
#ifdef listenfdET
				// 因为是ET模式，当listenfd上有事件发生，epoll_wait只通知一次
				// 所以需要用循环将监听队列中的客户连接一次性全部受理
                while (1) {
					// accept返回新的文件描述符connfd用于收发数据
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0) {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_connection::_user_count >= MAX_FD) {
						// 客户数量已达到上线，向新客户发送服务器繁忙信息，并关闭当前connfd
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
					// 将connfd注册到epoll内核事件表中
                    users[connfd].init(connfd, client_address);

                    // 初始化client_data数据
					// 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到时间堆中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->timeout_callback = cb_func;
					timer->expire = std::chrono::high_resolution_clock::now() + 3*std::chrono::seconds(TIMESLOT);
                    users_timer[connfd].timer = timer;
                    timer_manager.push_timer(timer);
                }
                continue;
#endif
            }

			// 如果有异常，就直接关闭客户连接，并删除该客户对应的定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
				if (timer) {
					timer->timeout_callback(&users_timer[sockfd]);
					/* timer_manager.del_timer(timer); */
				}
            }

            // 处理超时或终止服务信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
				int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
				for (int i = 0; i < ret; ++i) {
					switch (signals[i]) {
						case SIGALRM: { timeout = true; break; } // 触发超时信号
						case SIGTERM: { stop_server = true; break; } // 触发终止服务信号
					}
				}
            }

            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once()) {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    log::get_instance()->flush();
                    // 若监测到读事件，将该事件放入请求队列
                    pool->add_task([users, sockfd](){ users[sockfd].process(); });

                    // 若有数据传输，则将定时器往后延迟3个单位（15s），并调整定时器在堆中的位置
                    if (timer) {
						timer->expire = std::chrono::high_resolution_clock::now() + 3*std::chrono::seconds(TIMESLOT);
                        LOG_INFO("%s", "adjust timer once");
                        log::get_instance()->flush();
						// 由于延长了定时器的超时时间，所以需要调整定时器在堆中的位置
                        timer_manager.adjust_timer(timer);
                    }
                }
                else if (timer) {
					timer->timeout_callback(&users_timer[sockfd]);
					timer_manager.del_timer(timer);
                }
            }

			// 处理写入数据至客户连接
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer) {
						timer->expire = std::chrono::high_resolution_clock::now() + 3*std::chrono::seconds(TIMESLOT);
                        LOG_INFO("%s", "adjust timer once");
                        log::get_instance()->flush();
                        timer_manager.adjust_timer(timer);
                    }
                }
                else if (timer) {
                    timer->timeout_callback(&users_timer[sockfd]);
                    /* timer_manager.del_timer(timer); */
                }
            }
        }
        if (timeout) { timer_handler(); timeout = false; }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
