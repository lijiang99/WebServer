#ifndef CONNECT_POOL_H
#define CONNECT_POOL_H

#include <mysql/mysql.h>
#include <queue>
#include <list>
#include <string>
#include <atomic>
#include <mutex>
#include <semaphore.h>

typedef bool _connection_pool_status_type;
const _connection_pool_status_type _pool_initialized = true;
const _connection_pool_status_type _pool_uninitialized = false;

// 数据库连接池
class connection_pool {
	private:
		typedef _connection_pool_status_type pool_status_type;

		std::string _host; // 主机地址
		std::string _user; // 数据库用户名
		std::size_t _port; // 数据库端口号
		std::string _password; // 数据库密码
		std::string _database; // 所使用的数据库名
		std::size_t _max_conn; // 最大连接数
		// 原子变量，用于判断连接池是否已经初始化
		std::atomic<pool_status_type> _pool_status;
		std::mutex _mutex; // 互斥锁
		sem_t _sem; // 信号量
		// 使用以双向链表为底层数据结构的队列构造连接池
		std::queue<MYSQL*, std::list<MYSQL*>> _conn_queue;

	private:
		// 使用单例模式，声明私有构造，并禁止拷贝操作
		connection_pool() : _pool_status(_pool_uninitialized) {} 
		connection_pool(const connection_pool &rhs) = delete;
		connection_pool& operator=(const connection_pool &rhs) = delete;
	
	public:
		// 静态成员函数，获取单例模式的实例
		static connection_pool* get_instance();
		// 初始化单例模式的实例
		void init(const std::string &host, const std::string &user, int port,
				const std::string &password, const std::string &database, int max_conn);
	
		// 从连接池中获取一条可用连接
		MYSQL* get_connection();
		// 释放当前连接（加入连接池）
		void put_connection(MYSQL *conn);

		// 销毁数据库连接池
		void destroy();
		// 析构函数，需要在内部销毁连接池
		~connection_pool() { destroy(); }
};

// 采用RAII机制，在构造函数中分配资源（从连接池获取连接）
// 在析构函数中释放资源（将连接重新加入连接池）
class sql_connection {
	private:
		MYSQL *_conn;
		connection_pool *_conn_pool;
	public:
		// 通过指向连接池的指针来分配连接，并用返回的连接初始化_conn
		// 同时为修改传入参数，需传递指针型参数，而连接本身也是指针
		// 所以形参类型应该为二级指针
		sql_connection(MYSQL **sql, connection_pool *conn_pool) {
			*sql = conn_pool->get_connection();
			_conn = *sql;
			_conn_pool = conn_pool;
		}
		// 通过指向连接池的指针来释放所管理的连接
		~sql_connection() { _conn_pool->put_connection(_conn); }
};

#endif
