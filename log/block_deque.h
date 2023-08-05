#ifndef BLOCK_DEQUE_H
#define BLOCK_DEQUE_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

typedef bool _block_deque_status_type;
const _block_deque_status_type _deque_open = true;
const _block_deque_status_type _deque_close = false;

// 阻塞队列，用于异步写入日志信息
template <typename T>
class block_deque {
	private:
		typedef _block_deque_status_type deque_status_type;

		// 队列状态（打开/关闭）
		deque_status_type _deque_status;
		// 底层容器使用双端队列
		std::deque<T> _deque;
		// 队列的最大容量
		std::size_t _max_capacity;
		// 保护队列的互斥锁
		std::mutex _mutex;
		// 用于生产者/消费者模型的条件变量，需要搭配互斥锁一起使用
		std::condition_variable _cond_producer;
		std::condition_variable _cond_consumer;
	
	public:
		// 构造函数
		explicit block_deque(int max_capacity = 1000);
		// 析构函数
		~block_deque() { close(); }

		// 获取阻塞队列的一些状态信息，其实就是加锁再转调用底层容器的接口，以确保线程安全
		// 成员函数内部的加锁操作会改变锁状态，所以不能声明为const成员函数
		std::size_t size() { std::lock_guard<std::mutex> lock(_mutex); return _deque.size(); }
		bool full() { std::lock_guard<std::mutex> lock(_mutex); return _deque.size() >= _max_capacity; }
		bool empty() { std::lock_guard<std::mutex> lock(_mutex); return _deque.empty(); }
		// 此处对_max_capacity执行只读操作，且_max_capacity初始化后就不会改变
		// 所以该成员函数无需加锁，且可以声明为const成员函数
		std::size_t max_capacity() const { return _max_capacity; }

		// 获取队列的头部和尾部元素
		T front() { std::lock_guard<std::mutex> lock(_mutex); return _deque.front(); }
		T back() { std::lock_guard<std::mutex> lock(_mutex); return _deque.back(); }

		// 队列的添加/删除元素的操作
		void push_front(const T &item);
		void push_back(const T &item);
		bool pop(T &item);
		bool pop(T &item, int timeout);
		void clear() { std::lock_guard<std::mutex> lock(_mutex); _deque.clear(); }

		// 手动刷新，尝试唤醒消费者处理任务
		void flush() { _cond_consumer.notify_one(); }
		// 关闭队列
		void close();
};

// 构造函数
template <typename T>
block_deque<T>::block_deque(int max_capacity) : _deque_status(_deque_open) {
	if (max_capacity <= 0)
		throw std::runtime_error("invalid number of max_capacity");
	_max_capacity = static_cast<std::size_t>(max_capacity);
}

// 在队列头部添加元素
template <typename T>
void block_deque<T>::push_front(const T &item) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.size() >= _max_capacity)
		_cond_producer.wait(lock);
	_deque.push_front(item);
	_cond_consumer.notify_one();
}

// 在队列尾部添加元素
template <typename T>
void block_deque<T>::push_back(const T &item) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.size() >= _max_capacity)
		_cond_producer.wait(lock);
	_deque.push_back(item);
	_cond_consumer.notify_one();
}

// 取队列头部元素，并将元素值赋给引用传递的item
template <typename T>
bool block_deque<T>::pop(T &item) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.empty()) {
		_cond_consumer.wait(lock);
		if (_deque_status == _deque_close) return false;
	}
	item = _deque.front();
	_deque.pop_front();
	_cond_producer.notify_one();
	return true;
}

// 取队列头部元素，并将元素值赋给引用传递的item
// 指定超时时间（以秒为单位），若阻塞超时，则直接返回false
template <typename T>
bool block_deque<T>::pop(T &item, int timeout) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.empty()) {
		if (_cond_consumer.wait_for(lock, std::chrono::seconds(timeout))
				== std::cv_status::timeout) return false;
		if (_deque_status == _deque_close) return false;
	}
	item = _deque.front();
	_deque.pop_front();
	_cond_producer.notify_one();
	return true;
}

// 关闭队列，并唤醒所有阻塞的线程（生产者/消费者）
template <typename T>
void block_deque<T>::close() {
	std::unique_lock<std::mutex> lock(_mutex);
	_deque.clear();
	_deque_status = _deque_close;
	lock.unlock();
	_cond_producer.notify_all();
	_cond_consumer.notify_all();
}

#endif
