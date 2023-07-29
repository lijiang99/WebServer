#include <mysql/mysql.h>
#include <atomic>
#include <mutex>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include "connection_pool.h"

// 全局的数据库连接池配置文件路径
#define _CONFIG_PATH "./connection_pool.cnf"

// 默认初始化原子对象
std::atomic<connection_pool*> connection_pool::m_instance;
std::mutex connection_pool::m_mutex;

// 采用单例模式（懒汉式），并使用双检查锁确保线程安全和效率。
connection_pool* connection_pool::get_instance() {
	connection_pool* tmp = m_instance.load(std::memory_order_relaxed);
	// 设置内存屏障进行内存保护，确保tmp被赋值时不会被reorder
	std::atomic_thread_fence(std::memory_order_acquire);
	if (tmp == nullptr) {
		// 双检查锁
		std::lock_guard<std::mutex> lock(m_mutex);
		tmp = m_instance.load(std::memory_order_relaxed);
		if (tmp == nullptr) {
			// 初始化全局唯一的单例对象
			tmp = new connection_pool();
			// 释放内存屏障
			std::atomic_thread_fence(std::memory_order_release);
			m_instance.store(tmp, std::memory_order_relaxed);
		}
	}
	return tmp;
}

// 根据配置文件中的信息初始化数据库连接池
connection_pool::connection_pool() {
#ifndef NDEBUG
	std::cout << "initialize connection pool..." << std::endl;
#endif
	std::ifstream ifile(_CONFIG_PATH);
	std::string line, config;

	// 读取配置文件信息
	while (std::getline(ifile, line)) {
		std::istringstream input(line);
		if (input >> config) {
			if (config == "host") input >> _host;
			else if (config == "user") input >> _user;
			else if (config == "port") input >> _port;
			else if (config == "password") input >> _password;
			else if (config == "database") input >> _database;
			else if (config == "max_conn") input >> _max_conn;
		}
	}

	// 根据最大连接数预分配连接并添加到链表中
	for (std::size_t i = 0; i < _max_conn; ++i) {
		MYSQL* conn = nullptr;

		// 初始化连接
		conn = mysql_init(conn);
		if (conn == nullptr) {
			std::cout << "Error: " << mysql_error(conn);
			exit(1);
		}

		// 建立一条实际的数据库连接
		conn = mysql_real_connect(conn, _host.c_str(), _user.c_str(),
				_password.c_str(), _database.c_str(), _port, nullptr, 0);
		if (conn == nullptr) {
			std::cout << "Error: " << mysql_error(conn);
			exit(1);
		}

		// 向连接池中加入一条连接
		_conn_list.push_back(conn);
	}
	// 信号量初始化为最大连接数
	sem_init(&_sem, 0, _max_conn);
#ifndef NDEBUG
	std::cout << "** host: " << _host << std::endl;
	std::cout << "** user: " << _user << std::endl;
	std::cout << "** port: " << _port << std::endl;
	std::cout << "** password: " << _password << std::endl;
	std::cout << "** database: " << _database << std::endl;
	std::cout << "** max_conn: " << _max_conn << std::endl;
	std::cout << "** connection pool size: " << _conn_list.size() << std::endl;
#endif
}

// 从连接池中获取一条可用连接
MYSQL* connection_pool::get_connection() {
	MYSQL *conn = nullptr;
	if (_conn_list.empty()) return nullptr;

	// 取出连接，信号量减1，相当于P操作，为0则阻塞等待
	sem_wait(&_sem);

	// 取链表首元素作为可用连接
	std::lock_guard<std::mutex> lock(m_mutex);
#ifndef NDEBUG
	std::cout << "get connection from pool..." << std::endl;
#endif
	conn = _conn_list.front();
	_conn_list.pop_front();
#ifndef NDEBUG
	std::cout << "** connection pool size: " << _conn_list.size() << std::endl;
#endif

	return conn;
}

// 释放当前连接（加入连接池）
void connection_pool::put_connection(MYSQL *conn) {
	if (conn == nullptr) {
#ifndef NDEBUG
		std::cout << "cannot put invalid connection" << std::endl;
#endif
		return;
	}
	// 将连接重新加入链表
	std::lock_guard<std::mutex> lock(m_mutex);
#ifndef NDEBUG
	std::cout << "put connection from pool..." << std::endl;
#endif
	_conn_list.push_back(conn);
	// 信号量加1，相当于V操作
	sem_post(&_sem);
#ifndef NDEBUG
	std::cout << "** connection pool size: " << _conn_list.size() << std::endl;
#endif
}

// 销毁数据库连接池
void connection_pool::destroy() {
#ifndef NDEBUG
	std::cout << "destroy connection pool..." << std::endl;
#endif
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!_conn_list.empty()) {
		// 遍历链表，关闭数据库连接
		for (auto iter = _conn_list.begin(); iter != _conn_list.end(); ++iter)
			mysql_close(*iter);
		// 清空链表
		_conn_list.clear();
	}
#ifndef NDEBUG
	std::cout << "** connection pool size: " << _conn_list.size() << std::endl;
#endif
}
