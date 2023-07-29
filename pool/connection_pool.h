#ifndef CONNECT_POOL_H
#define CONNECT_POOL_H

#include <mysql/mysql.h>
#include <list>
#include <string>
#include <atomic>
#include <mutex>
#include <semaphore.h>

// 数据库连接池
class connection_pool {
	private:
		std::string _host; // 主机地址
		std::string _user; // 数据库用户名
		std::size_t _port; // 数据库端口号
		std::string _password; // 数据库密码
		std::string _database; // 所使用的数据库名
		std::size_t _max_conn; // 最大连接数
		sem_t _sem; // 信号量
		std::list<MYSQL*> _conn_list; // 连接池，以链表作为底层结构

	private:
		// 使用单例模式，声明私有构造，并禁止拷贝操作
		connection_pool(); 
		connection_pool(const connection_pool& rhs) = delete;
		connection_pool& operator=(const connection_pool& rhs) = delete;
	
	public:
		// 获取单例模式的实例
		static connection_pool* get_instance();
		// 原子对象和互斥锁
		// ****
		// ****原子对象所管理的指针会指向堆内存，如何释放需要考虑
		// ****
		static std::atomic<connection_pool*> m_instance;
		static std::mutex m_mutex;
	
	public:
		// 从连接池中获取一条可用连接
		MYSQL* get_connection();
		// 释放当前连接（加入连接池）
		void put_connection(MYSQL* conn);

		// 销毁数据库连接池
		void destroy();
		// 析构函数，需要在内部销毁连接池
		~connection_pool() { destroy(); }
};

// 采用RAII机制，在构造函数中分配资源（从连接池获取连接）
// 在析构函数中释放资源（将连接重新加入连接池）
class connection_RAII {
	private:
		MYSQL* _conn;
		connection_pool* _conn_pool;
	public:
		// 通过指向连接池的指针来分配连接，并用返回的连接初始化_conn
		// 同时为修改传入参数，需传递指针型参数，而连接本身也是指针
		// 所以形参类型应该为二级指针
		connection_RAII(MYSQL** sql, connection_pool* conn_pool) {
			*sql = conn_pool->get_connection();
			_conn = *sql;
			_conn_pool = conn_pool;
		}
		// 通过指向连接池的指针来释放所管理的连接
		~connection_RAII() { _conn_pool->put_connection(_conn); }
};

#endif
