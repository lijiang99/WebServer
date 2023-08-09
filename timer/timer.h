#ifndef TIMER_H
#define TIMER_H

#include <netinet/in.h>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#ifndef NDEBUG
#include <iostream>
#include <sstream>
#include <iomanip>

// 将系统时间格式化为字符串（精确到毫秒）
static std::string get_format_time(const std::chrono::system_clock::time_point &tp,
		const std::string &fmt = "%Y-%m-%d %H:%M:%S") {
	std::ostringstream fortmat_time;
	std::time_t tmp_tm = std::chrono::system_clock::to_time_t(tp);
	std::chrono::milliseconds cs = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
	fortmat_time << std::put_time(std::localtime(&tmp_tm), fmt.c_str())
		<< "." << std::setfill('0') << std::setw(3) << cs.count();
	return fortmat_time.str();
}
#endif

// 定时器的时间类型和用于判断是否超时的时间间隔类型（精确到毫秒）
typedef std::chrono::high_resolution_clock::time_point expire_type;
typedef std::chrono::milliseconds interval_type;

class util_timer; // 前向声明

// 用户数据，绑定socket和定时器
struct client_data {
	sockaddr_in address;
	int sockfd;
	util_timer *timer;
};

// 定时器类
struct util_timer {
	typedef void(*callback_type)(client_data*);

	expire_type expire; // 定时器的超时时间（绝对时间）
	callback_type timeout_callback; // 定时器的回调函数
	client_data* user_data; // 用户数据
};

// 模板函数，比较两个指针所指向的定时器的超时时间，作为时间堆中的比较准则
template <typename Timer>
struct greater_exprie {
	bool operator()(const Timer* lhs, const Timer* rhs) { return lhs->expire > rhs->expire; }
};

// 时间堆类（应以最小堆实现）
template <typename Timer, typename Compare = greater_exprie<Timer>>
class timer_heap {
	private:
		typedef std::vector<Timer*> heap_type;
		/* typedef bool(*compare_type)(const Timer*, const Timer*); */

		heap_type *_heap; // 以vector作为时间堆的底层容器
		Compare _compare; // 用于调整堆结构的比较准则

	public:
		// 构造函数，初始化一个空堆
		timer_heap(Compare comp = Compare()) : _compare(comp) {
#ifndef NDEBUG
			std::cout << "\ninitialize timer heap..." << std::endl;
#endif
			_heap = new heap_type();
#ifndef NDEBUG
			std::cout << "** heap size => " << _heap->size() << std::endl;
#endif
		}
		// 析构函数，销毁时间堆
		~timer_heap() {
#ifndef NDEBUG
			std::cout << "\ndestroy timer heap..." << std::endl;
#endif
			for (typename heap_type::size_type i = 0; i < _heap->size(); ++i) {
#ifndef NDEBUG
				std::cout << "** deallocate timer expire => " << get_format_time(_heap->at(i)->expire) << std::endl;
#endif
				delete (*_heap)[i];
			}
			delete _heap;
#ifndef NDEBUG
			std::cout << "** deallocate timer heap" << std::endl;
#endif
		}

		// 向堆中添加一个定时器
		inline void push_timer(Timer *timer);
		// 删除堆顶的定时器
		inline void pop_timer();

		// 心搏函数
		void tick();
};

// 向堆中添加一个定时器
template <typename Timer, typename Compare>
void timer_heap<Timer, Compare>::push_timer(Timer *timer) {
#ifndef NDEBUG
	std::cout << "\nadd timer into timer heap..." << std::endl;
#endif
	// 需先使用push_back将timer添加到底层vector的尾部，再使用push_heap完成上溯
	_heap->push_back(timer);
	std::push_heap(_heap->begin(), _heap->end(), _compare);
#ifndef NDEBUG
	std::cout << "** (success) timer expire => " << get_format_time(timer->expire) << std::endl;
#endif
}

// 删除堆顶的定时器
template <typename Timer, typename Compare>
void timer_heap<Timer, Compare>::pop_timer() {
#ifndef NDEBUG
	std::cout << "\npop timer from timer heap..." << std::endl;
#endif
	// 需先使用pop_heap将堆顶元素置于底层vector的尾部，并调整好堆结构
	// 再通过底层vector的pop_back实现真正的删除（删除原先的堆顶元素）
	std::pop_heap(_heap->begin(), _heap->end(), _compare);
#ifndef NDEBUG
	std::cout << "** (success) timer expire => " << get_format_time(_heap->back()->expire) << std::endl;
#endif
	_heap->pop_back();
}

// 心搏函数，处理到期的定时器
template <typename Timer, typename Compare>
void timer_heap<Timer, Compare>::tick() {
	// 若堆中尚有元素，则通过循环来处理所有到期的定时器
	while (!_heap->empty()) {
#ifndef NDEBUG
		std::cout << "\ntick to process..." << std::endl;
#endif
		typename heap_type::iterator iter = _heap->begin();
		// 计算超时时间与当前时间间隔
		expire_type now = std::chrono::high_resolution_clock::now();
		interval_type interval_time = std::chrono::duration_cast<interval_type>((*iter)->expire - now);
#ifndef NDEBUG
		std::cout << "** now time => " << get_format_time(now) << std::endl;
#endif
		// 若间隔大于0则表示尚未超时
		if (interval_time.count() > 0) {
#ifndef NDEBUG
			std::cout << "** timer lefts " << interval_time.count() << " milliseconds" << std::endl;
#endif
			break;
		}
#ifndef NDEBUG
		std::cout << "** timer expired => " << get_format_time((*iter)->expire) << std::endl;
#endif
		(*iter)->timeout_callback((*iter)->user_data);
		pop_timer();
	}
}

#endif
