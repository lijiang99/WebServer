#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include "log.h"

#ifndef NDEBUG
#include <iostream>
#endif

// 采用单例模式（懒汉式），并使用局部静态变量确保线程安全
log* log::get_instance() {
	static log logger;
	return &logger;
}

// 根据初始化状态判断是否真的需要初始化全局唯一的日志对象，并确保线程安全
void log::init(const std::string &dir_path, int max_lines, int max_queue_capacity) {
	// 利用原子变量和CAS操作判断日志对象是否已经初始化过
	log_status_type expected = _log_uninitialized;
	if (!_log_status.compare_exchange_weak(expected, _log_initialized))
		throw std::runtime_error("logger already initialized");

#ifndef NDEBUG
	std::cout << "\ninitialize logger..." << std::endl;
#endif

	if (dir_path.empty())
		throw std::runtime_error("invalid directory path for saving the log file");
	_dir_path = (dir_path[dir_path.size()-1] == '/' ? dir_path : dir_path + '/');

	if (max_lines <= 0)
		throw std::runtime_error("invalid number of max_lines");
	// 单个日志文件可存储的最大行数
	_max_lines = static_cast<std::size_t>(max_lines);

	// 若设置了阻塞队列大小，则表示采取异步日志模式
	// 否则默认为0（或设置小于0），表示采取同步日志模式
	if (max_queue_capacity > 0) {
		_write_mode = _async_write;
		if (!(_log_queue = new log_queue_type(max_queue_capacity)))
			throw std::runtime_error("failed to allocate memory for log queue");
		// 创建写线程，专门用于异步写入日志信息
		std::thread(&log::async_write_log, this).detach();
	}

	// 获取当前系统时间，并提取天数
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	_days = std::chrono::duration_cast<std::chrono::days>(now.time_since_epoch());

	// 设置日志文件路径并设置输出文件流对象
	_file_path = _dir_path + "WebServer_" + get_format_time(now, "%Y-%m-%d_%H:%M:%S") + ".log";
	_file_output.open(_file_path, std::ofstream::app);
	if (!_file_output) throw std::runtime_error("failed to open '" + _file_path + "'");

#ifndef NDEBUG
	std::cout << "** file_path => " << _file_path << std::endl;
	std::cout << "** max_lines => " << _max_lines << std::endl;
	std::cout << "** cnt_lines => " << _cnt_lines << std::endl;
	std::cout << "** days => " << _days.count() << std::endl;
	std::cout << "** max_queue_capacity => " << max_queue_capacity << std::endl;
	std::cout << "** write mode => " << (_write_mode == _async_write ? "async" : "sync") << std::endl;
#endif
}

// 将系统时间格式化为字符串（精确到微秒）
std::string log::get_format_time(const std::chrono::system_clock::time_point &tp, const std::string &fmt) {
	std::ostringstream fortmat_time;
	std::time_t tmp_tm = std::chrono::system_clock::to_time_t(tp);
	std::chrono::microseconds cs = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()) % 1000000;
	fortmat_time << std::put_time(std::localtime(&tmp_tm), fmt.c_str())
		<< "." << std::setfill('0') << std::setw(6) << cs.count();
	return fortmat_time.str();
}

// 作为写线程的回调函数，用于异步写入日志信息
void log::async_write_log() {
	std::string message;
	// 从阻塞队列中取出日志信息并写入文件输出流对象
	while (_log_queue->pop(message))  {
		std::lock_guard<std::mutex> lock(_mutex);
		_file_output << message;
	}
}
