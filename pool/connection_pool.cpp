#include <mysql/mysql.h>
#include <mutex>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include "connection_pool.h"

// 全局的数据库连接池配置文件路径
#define _CONFIG_PATH "./connection_pool.cnf"

// 采用单例模式（懒汉式），并使用局部静态变量确保线程安全
connection_pool* connection_pool::get_instance() {
	static connection_pool conn_pool;
	return &conn_pool;
}

// 根据配置文件中的信息初始化数据库连接池
connection_pool::connection_pool() {
#ifndef NDEBUG
	std::cout << "\ninitialize connection pool..." << std::endl;
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
#ifndef NDEBUG
	std::cout << "** host => " << _host << std::endl;
	std::cout << "** user => " << _user << std::endl;
	std::cout << "** port => " << _port << std::endl;
	std::cout << "** password => " << _password << std::endl;
	std::cout << "** database => " << _database << std::endl;
	std::cout << "** max_conn => " << _max_conn << std::endl;
#endif

	// 根据最大连接数预分配连接并添加到队列中
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
		_conn_queue.push(conn);
#ifndef NDEBUG
		std::cout << "** open mysql connection => " << conn << std::endl;
#endif
	}
	// 信号量初始化为最大连接数
	sem_init(&_sem, 0, _max_conn);
#ifndef NDEBUG
	std::cout << "** (success) connection pool size => " << _conn_queue.size() << std::endl;
#endif
}

// 从连接池中获取一条可用连接
MYSQL* connection_pool::get_connection() {
	if (_conn_queue.empty()) return nullptr;

	// 取出连接，信号量减1，相当于P操作，为0则阻塞等待
	sem_wait(&_sem);

	// 取队列首元素作为可用连接
	std::lock_guard<std::mutex> lock(_mutex);
#ifndef NDEBUG
	std::cout << "\nget connection from pool..." << std::endl;
#endif
	MYSQL *conn = _conn_queue.front();
	_conn_queue.pop();
#ifndef NDEBUG
	std::cout << "** get mysql connection => " << conn << std::endl;
	std::cout << "** (success) connection pool size => " << _conn_queue.size() << std::endl;
#endif

	return conn;
}

// 释放当前连接（加入连接池）
void connection_pool::put_connection(MYSQL *conn) {
	if (conn == nullptr) {
#ifndef NDEBUG
		std::cout << "\nput connection to pool..." << std::endl;
		std::cout << "** (failure) cannot put invalid connection" << std::endl;
#endif
		return;
	}
	// 将连接重新加入队列中
	std::lock_guard<std::mutex> lock(_mutex);
#ifndef NDEBUG
	std::cout << "\nput connection to pool..." << std::endl;
#endif
	_conn_queue.push(conn);
	// 信号量加1，相当于V操作
	sem_post(&_sem);
#ifndef NDEBUG
	std::cout << "** put mysql connection => " << conn << std::endl;
	std::cout << "** (success) connection pool size => " << _conn_queue.size() << std::endl;
#endif
}

// 销毁数据库连接池
void connection_pool::destroy() {
#ifndef NDEBUG
	std::cout << "\ndestroy connection pool..." << std::endl;
#endif
	std::lock_guard<std::mutex> lock(_mutex);
	// 关闭队列中所有的数据库连接，并清空队列
	while (!_conn_queue.empty()) {
#ifndef NDEBUG
		std::cout << "** close mysql connection => " << _conn_queue.front() << std::endl;
#endif
		mysql_close(_conn_queue.front());
		_conn_queue.pop();
	}
	mysql_library_end();
#ifndef NDEBUG
	std::cout << "** (success) connection pool size => " << _conn_queue.size() << std::endl;
#endif
}
