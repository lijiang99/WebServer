#ifndef LOG_H
#define LOG_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifndef NDEBUG
#include <iostream>
#endif

// 定义阻塞队列状态类型
typedef bool _block_queue_status_type;
const _block_queue_status_type _queue_open = true;
const _block_queue_status_type _queue_close = false;

// 阻塞队列，用于异步写入日志信息
template <typename T>
class _block_queue {
	private:
		typedef _block_queue_status_type queue_status_type;

		// 队列状态（打开/关闭）
		queue_status_type _queue_status;
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
		explicit _block_queue(int max_capacity = 1000);
		// 析构函数
		~_block_queue();

		// 队列的添加/删除元素的操作
		void push(const T &item);
		bool pop(T &item);
};

// 构造函数
template <typename T>
_block_queue<T>::_block_queue(int max_capacity) : _queue_status(_queue_open) {
	if (max_capacity <= 0)
		throw std::runtime_error("invalid number of max_capacity");
	_max_capacity = static_cast<std::size_t>(max_capacity);
}

// 析构函数，关闭队列并唤醒所有阻塞的线程（生产者/消费者）
template <typename T>
_block_queue<T>::~_block_queue() {
	std::unique_lock<std::mutex> lock(_mutex);
	_deque.clear();
	_queue_status = _queue_close;
	lock.unlock();
	_cond_producer.notify_all();
	_cond_consumer.notify_all();
}

// 在队列尾部添加元素
template <typename T>
void _block_queue<T>::push(const T &item) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.size() >= _max_capacity)
		_cond_producer.wait(lock);
	_deque.push_back(item);
	_cond_consumer.notify_one();
}

// 取出队列头部元素，并将元素值赋给引用传递的item
template <typename T>
bool _block_queue<T>::pop(T &item) {
	std::unique_lock<std::mutex> lock(_mutex);
	while (_deque.empty()) {
		_cond_consumer.wait(lock);
		if (_queue_status == _queue_close) return false;
	}
	item = _deque.front();
	_deque.pop_front();
	_cond_producer.notify_one();
	return true;
}

// 定义日志初始化状态类型
typedef bool _log_status_type;
const _log_status_type _log_initialized = true;
const _log_status_type _log_uninitialized = false;

// 定义日志写入模式类型
typedef bool _log_write_mode_type;
const _log_write_mode_type _async_write = true;
const _log_write_mode_type _sync_write = false;

// 枚举类型，定义四种日志信息级别
enum class LOG_LEVEL { DEBUG, INFO, WARN, ERROR };

// 日志类，可用于同步/异步写入日志
class log {
	private:
		typedef _log_status_type log_status_type;
		typedef _log_write_mode_type write_mode_type;
		typedef _block_queue<std::string> log_queue_type;

		std::string _dir_path; // 路径名
		std::string _file_path; // 日志文件路径
		std::size_t _max_lines; // 最大行数
		std::size_t _cnt_lines; // 行数记录
		std::chrono::days _days; // 记录当前时间是哪一天
		write_mode_type _write_mode; // 写入方式
		log_queue_type *_log_queue; // 阻塞队列
		std::ofstream _file_output; // 文件输出流
		// 原子变量，用于判断日志单例是否已经初始化
		std::atomic<log_status_type> _log_status;
		std::mutex _mutex; // 互斥锁

	private:
		// 使用单例模式，声明私有构造，并禁止拷贝操作
		log() : _cnt_lines(0), _log_queue(nullptr), _log_status(_log_uninitialized) {}
		log(const log &rhs) = delete;
		log& operator=(const log &rhs) = delete;

	private:
		// 将系统时间格式化为字符串（精确到微秒）
		static std::string get_format_time(const std::chrono::system_clock::time_point &tp,
				const std::string &fmt = "%Y-%m-%d %H:%M:%S");
		// 异步写入日志信息
		void async_write_log();
		
		// 模板成员函数，将单个对象写入字符串输出流，将作为重载的可变参版本的递归终止条件
		// 即向文件输出流对象写入最后一个数据，并添加换行符表示该日志消息结束
		template <typename T>
		std::ostringstream& to_ostringstream(std::ostringstream &os, const T &t) { os << t << '\n'; return os; }

		// 可变参模板，将任意类型的多个对象写入字符串输出流
		template <typename T, typename... Args>
		std::ostringstream& to_ostringstream(std::ostringstream &os, const T &t, const Args &...rest);

	public:
		// 静态成员函数，获取单例模式的实例
		static log* get_instance();
		// 初始化单例模式的实例
		void init(const std::string &dir_path, int max_lines, int max_queue_capacity = 0);
		// 可变参模板，根据写入方式，向文件输出流对象同步/异步写入日志信息
		template <typename... Args> void write_log(LOG_LEVEL level, const Args &...rest);
		// 手动刷新文件输出流缓冲区
		void flush() { _file_output << std::flush; }
		// 析构函数，需要在内部销毁阻塞队列，并关闭文件输出流
		~log() {
#ifndef NDEBUG
			std::cout << "\ndestroy logger..." << std::endl;
#endif
			if (_log_queue) {
#ifndef NDEBUG
			std::cout << "** deallocate log queue" << std::endl;
#endif
				delete _log_queue;
			}
			_file_output.close();
#ifndef NDEBUG
			std::cout << "** close file => " << _file_path << std::endl;
#endif
		}
};

// 可变参模板，以递归的方式将任意类型的多个对象写入字符串输出流
// 递归终止条件为参数包被分解为只剩一个参数的状态，此时调用非可变参版本的to_ostringstream
template <typename T, typename... Args>
std::ostringstream& log::to_ostringstream(std::ostringstream &os, const T &t, const Args &...rest) {
	os << t << " ";
	return to_ostringstream(os, rest...);
}

// 可变参模板，可设置日志信息级别，并将任意类型的多个对象组合成字符串形式的日志信息
template <typename ...Args>
void log::write_log(LOG_LEVEL level, const Args &...rest) {
	// 获取当前系统时间，并提取天数
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	std::chrono::days tmp_days = std::chrono::duration_cast<std::chrono::days>(now.time_since_epoch());

	// 若当前天数和先前设置的天数不同，或日志文件行数已经达到最大值
	// 则刷新先前的缓冲区内容至日志文件，并创建新的日志文件
	std::unique_lock<std::mutex> lock(_mutex);
	++_cnt_lines;
	if (tmp_days != _days || _cnt_lines >= _max_lines) {
		// 刷新缓冲区内容至日志文件
		_file_output << std::flush;
		_file_output.close();
		// 设置新日志文件路径并重置输出文件流对象
		_file_path = _dir_path + "WebServer_" + get_format_time(now, "%Y-%m-%d_%H:%M:%S") + ".log";
		_file_output.open(_file_path, std::ofstream::app);
		if (!_file_output) throw std::runtime_error("failed to open '" + _file_path + "'");
		_days = tmp_days;
		_cnt_lines = 0;
	}
	lock.unlock();

	// 字符串输出流，用于暂存将要写入日志文件的日志消息
	std::ostringstream message;
	// 设置消息头中的时间部分
	message << get_format_time(now) << " ";
	// 设置消息头中的消息等级
	switch (level) {
		case LOG_LEVEL::DEBUG:
			message << "[DEBUG]: ";
			break;
		case LOG_LEVEL::INFO:
			message << "[INFO]: ";
			break;
		case LOG_LEVEL::WARN:
			message << "[WARN]: ";
			break;
		case LOG_LEVEL::ERROR:
			message << "[ERROR]: ";
			break;
		default:
			message << "[INFO]: ";
			break;
	}

	// 实例化可变参模板，将任意类型的多个对象所组成的消息体写入字符串输出流
	to_ostringstream(message, rest...);

	// 若为异步写入，则将消息加入阻塞队列，否则为同步写入，则将消息直接输出到文件流
	if (_write_mode == _async_write) _log_queue->push(message.str());
	else { lock.lock(); _file_output << message.str(); lock.unlock(); }
}

// 宏函数，使写入日志消息更加便捷
#define LOG_DEBUG(...) log::get_instance()->write_log(LOG_LEVEL::DEBUG, ##__VA_ARGS__)
#define LOG_INFO(...) log::get_instance()->write_log(LOG_LEVEL::INFO, ##__VA_ARGS__)
#define LOG_WARN(...) log::get_instance()->write_log(LOG_LEVEL::WARN, ##__VA_ARGS__)
#define LOG_ERROR(...) log::get_instance()->write_log(LOG_LEVEL::ERROR, ##__VA_ARGS__)
#define LOG_FLUSH() log::get_instance()->flush();

#endif
