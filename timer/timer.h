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
	std::size_t id; // 记录定时器在堆中的索引位置，用于快速定位
};

// 模板函数，比较两个指针所指向的定时器的超时时间，作为时间堆中的比较准则
/* template <typename Timer> */
/* struct greater_exprie { */
/* 	bool operator()(const Timer* lhs, const Timer* rhs) { return lhs->expire > rhs->expire; } */
/* }; */

// 时间堆类（应以最小堆实现）
/* template <typename Timer, typename Compare = greater_exprie<Timer>> */
template <typename Timer>
class timer_heap {
	private:
		typedef std::vector<Timer*> heap_type;
		/* typedef bool(*compare_type)(const Timer*, const Timer*); */

		heap_type _heap; // 以vector作为时间堆的底层容器
		/* Compare _compare; // 用于调整堆结构的比较准则 */
	
	private:
		// 交换堆中的两个元素时需要交换定时器的底层id
		void swap(Timer* &lhs, Timer* &rhs) { std::swap(lhs->id, rhs->id); std::swap(lhs, rhs); }
		// 上滤和下滤操作
		bool shift_up(std::size_t hole_idx);
		bool shift_down(std::size_t hole_idx);

	public:
		// 构造函数，初始化一个空堆
		/* timer_heap(Compare comp = Compare()) : _compare(comp) { */
		timer_heap() {
#ifndef NDEBUG
			std::cout << "\ninitialize timer heap..." << std::endl;
#endif
			/* _heap = new heap_type(); */
#ifndef NDEBUG
			std::cout << "** heap size => " << _heap.size() << std::endl;
#endif
		}
		// 析构函数，销毁时间堆
		~timer_heap() {
#ifndef NDEBUG
			std::cout << "\ndestroy timer heap..." << std::endl;
#endif
			/* for (typename heap_type::size_type i = 0; i < _heap->size(); ++i) { */
			for (typename heap_type::reverse_iterator iter = _heap.rbegin(); iter < _heap.rend(); ++iter) {
#ifndef NDEBUG
				std::cout << "** deallocate timer expire => " << get_format_time((*iter)->expire) << std::endl;
#endif
				delete *iter;
			}
			/* delete _heap; */
#ifndef NDEBUG
			std::cout << "** deallocate timer heap" << std::endl;
#endif
		}

		// 向堆中添加一个定时器
		void push_timer(Timer *timer);
		// 删除堆顶的定时器
		void pop_timer();
		// 删除堆中任意位置的定时器
		void del_timer(Timer *timer);
		// 调整堆中任意位置的定时器
		void adjust_timer(Timer *timer);

		// 心搏函数
		void tick();
};

// 在堆中执行上滤操作
template <typename Timer>
bool timer_heap<Timer>::shift_up(std::size_t hole_idx) {
	std::size_t tmp_idx = hole_idx;
	std::size_t parent_idx = (hole_idx - 1) >> 1;
	while (hole_idx > 0) {
		if (_heap[parent_idx]->expire < _heap[hole_idx]->expire) break;
		swap(_heap[parent_idx], _heap[hole_idx]);
		hole_idx = parent_idx;
		parent_idx = (hole_idx - 1) >> 1;
	}
	return tmp_idx ^ hole_idx;
}

// 在堆中执行下滤操作
template <typename Timer>
bool timer_heap<Timer>::shift_down(std::size_t hole_idx) {
	std::size_t tmp_idx = hole_idx;
	std::size_t child_idx = hole_idx * 2 + 1;
	while (child_idx < _heap.size()) {
		if (child_idx + 1 < _heap.size() && _heap[child_idx]->expire > _heap[child_idx+1]->expire) ++child_idx;
		if (_heap[hole_idx]->expire < _heap[child_idx]->expire) break;
		swap(_heap[hole_idx], _heap[child_idx]);
		hole_idx = child_idx;
		child_idx = hole_idx * 2 + 1;
	}
	return tmp_idx ^ hole_idx;
}

// 向堆中添加一个定时器
/* template <typename Timer, typename Compare> */
template <typename Timer>
void timer_heap<Timer>::push_timer(Timer *timer) {
#ifndef NDEBUG
	std::cout << "\npush timer into timer heap..." << std::endl;
#endif
	// 新定时器的初始id为尾元素后一个索引位置
	timer->id = _heap.size();
	// 先使用push_back将timer添加到底层vector的尾部，再使用shift_up完成上滤
	_heap.push_back(timer);
	shift_up(timer->id);
	/* std::push_heap(_heap->begin(), _heap->end(), _compare); */
#ifndef NDEBUG
	for (std::size_t i = 0; i < _heap.size(); ++i) {
		std::cout << get_format_time(_heap[i]->expire) << " id: " << _heap[i]->id << std::endl;
	}
#endif
}

// 删除堆顶的定时器
template <typename Timer>
void timer_heap<Timer>::pop_timer() {
#ifndef NDEBUG
	std::cout << "\npop timer from timer heap..." << std::endl;
#endif
	// 先交换首尾元素，然后删除尾元素（原先的堆顶元素）
	// 再对新的堆顶元素执行下滤操作
	swap(_heap.front(), _heap.back());
	delete _heap.back();
	_heap.pop_back();
	shift_down(0);
#ifndef NDEBUG
	for (std::size_t i = 0; i < _heap.size(); ++i) {
		std::cout << get_format_time(_heap[i]->expire) << " id: " << _heap[i]->id << std::endl;
	}
#endif
}

// 删除堆中任意位置的定时器
template <typename Timer>
void timer_heap<Timer>::del_timer(Timer *timer) {
#ifndef NDEBUG
	std::cout << "\ndelete timer from timer heap..." << std::endl;
#endif
	// 首先根据id快速定位定时器在堆中的位置
	std::size_t hole_idx = timer->id;
	// 将当前位置的元素与尾元素交换位置
	swap(_heap[hole_idx], _heap.back());
	// 删除尾元素（原先堆中任意位置的元素）
	delete _heap.back();
	_heap.pop_back();
	// 如果上滤不成功就尝试下滤
	if (!shift_up(hole_idx)) shift_down(hole_idx);
#ifndef NDEBUG
	for (std::size_t i = 0; i < _heap.size(); ++i) {
		std::cout << get_format_time(_heap[i]->expire) << " id: " << _heap[i]->id << std::endl;
	}
#endif
}

// 调整堆中任意位置的定时器，一般用于延长定时器时间的情况
template <typename Timer>
void timer_heap<Timer>::adjust_timer(Timer *timer) {
#ifndef NDEBUG
	std::cout << "\nadjust timer from timer heap..." << std::endl;
#endif
	// 首先根据id快速定位定时器在堆中的位置
	std::size_t hole_idx = timer->id;
	// 如果下滤不成功就尝试上滤
	// 由于通常是用于延长定时器时间的
	// 所以对于最小堆来说，应该优先执行下滤
	if (!shift_down(hole_idx)) shift_up(hole_idx);
#ifndef NDEBUG
	for (std::size_t i = 0; i < _heap.size(); ++i) {
		std::cout << get_format_time(_heap[i]->expire) << " id: " << _heap[i]->id << std::endl;
	}
#endif
}

// 心搏函数，处理到期的定时器
template <typename Timer>
void timer_heap<Timer>::tick() {
	// 若堆中尚有元素，则通过循环来处理所有到期的定时器
	while (!_heap.empty()) {
#ifndef NDEBUG
		std::cout << "\ntick to process..." << std::endl;
#endif
		typename heap_type::iterator iter = _heap.begin();
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
