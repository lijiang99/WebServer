#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <functional>
#include <queue>
#include <list>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

#ifndef NDEBUG
#include <iostream>
#endif

typedef bool _thread_pool_status_type;
const _thread_pool_status_type _pool_startup = true;
const _thread_pool_status_type _pool_shutdown = false;

// 半同步/半反应堆模式的线程池
template <typename Callback>
class thread_pool {
	private:
		typedef _thread_pool_status_type pool_status_type;
		// 任务类型为仿函数对象（回调函数）
		typedef std::function<Callback> task_type;
		// 请求队列类型，以双向链表为底层容器的队列，每个元素均表示一个任务
		typedef std::queue<task_type, std::list<task_type>> task_queue_type;

		// 线程池状态（打开/关闭）
		pool_status_type _pool_status;
		// 线程池中线程数量
		std::size_t _thread_number;
		// 请求队列允许的最大请求数
		std::size_t _max_requests;
		// 指向请求队列的指针，队列中元素即工作线程需要竞争的共享资源
		task_queue_type *_task_queue;
		// 保护请求队列的互斥锁
		std::mutex _mutex;
		// 用于生产者/消费者（工作线程）模型的条件变量，需要搭配互斥锁一起使用
		std::condition_variable _cond_producer;
		std::condition_variable _cond_consumer;

	public:
		// 构造函数
		thread_pool(int thread_number, int max_requests);

		// 析构函数
		~thread_pool();

		// 向请求队列中添加任务
		void add_task(task_type &&task);

		// 工作线程从请求队列中拉取任务进行处理
		void worker();
};

// 构造函数，初始化线程池信息，并创建工作线程
template <typename Callback>
thread_pool<Callback>::thread_pool(int thread_number, int max_requests) : _pool_status(_pool_startup) {
#ifndef NDEBUG
		std::cout << "\ninitialize thread pool..." << std::endl;
#endif
	// 判断线程数和最大请求数是否合法
	if (thread_number <= 0 || max_requests <= 0)
		throw std::runtime_error("invalid number of threads or requests");
	// 分配请求队列，默认初始化为空队列
	if (!(_task_queue = new task_queue_type()))
		throw std::runtime_error("failed to allocate memory for task queue");

	_thread_number = static_cast<std::size_t>(thread_number);
	_max_requests = static_cast<std::size_t>(max_requests);

	// 创建工作线程，并将工作线程与主线程分离
	for (std::size_t i = 0; i < _thread_number; ++i) {
#ifndef NDEBUG
		std::cout << "** create the " << i << "-th thread" << std::endl;
#endif
		// 使用成员函数作为工作线程的回调函数需额外传递this指针
		std::thread(&thread_pool::worker, this).detach();
	}
#ifndef NDEBUG
	std::cout << "** " << (_pool_status ? "(startup)" : "(shutdown)")
		<< " thread pool size => " << _thread_number << std::endl;
#endif
}

// 析构函数，关闭并释放线程池
template <typename Callback>
thread_pool<Callback>::~thread_pool() {
#ifndef NDEBUG
	std::cout << "\ndestroy thread pool..." << std::endl;
#endif
	_pool_status = _pool_shutdown; delete _task_queue;
	_cond_producer.notify_all();
	// 唤醒所有阻塞的工作线程，由于此时线程池变成了关闭状态
	// 那么worker中的循环判断就不成立，因此工作线程退出
	_cond_consumer.notify_all();
#ifndef NDEBUG
	std::cout << "** " << (_pool_status ? "(startup)" : "(shutdown)")
		<< " thread pool size => 0" << std::endl;
#endif
}

// 向请求队列中添加任务，右值引用用于模板转发实参
// 函数形参为指向模板类型参数的右值引用，可保持实参的所有类型信息
template <typename Callback>
void thread_pool<Callback>::add_task(task_type &&task) {
	// 向请求队列中添加任务，需要对请求队列进行锁保护
	std::unique_lock<std::mutex> lock(_mutex);
#ifndef NDEBUG
	std::cout << "\nadd request into task queue..." << std::endl;
#endif
	// 若请求队列中的任务数超出最大请求数上限，应该延迟添加，而不应该拒绝请求
	// 即阻塞生产者线程，等待工作线程取出任务为请求队列腾出空间以添加新任务
	while (_task_queue->size() >= _max_requests) {
#ifndef NDEBUG
		std::cout << "** (wait) task queue size => " << _task_queue->size() << std::endl;
#endif
		// 若生产者线程此时处于加锁状态，则自动解锁，但不解除阻塞
		// 若生产者线程解除阻塞，则再次加锁，以执行临界区（while循环后面的代码）
		_cond_producer.wait(lock);
	}
	// 向请求队列中添加新任务，forward用于保持实参类型信息
	// emplace在队列尾部调用task的构造函数来添加一个任务，效率更高
	// 且会根据task的类型信息（左值/右值），调用不同的构造函数
	// 若为左值，则调用拷贝构造函数，若为右值，且task对象有移动构造
	// 则调用移动构造进一步提升效率，否则仍调用拷贝构造
	// 移动操作的存在条件：
	// 1. 自定义了移动构造和移动赋值，但此时编译器不会再默认合成拷贝操作
	// 2. 未自定义拷贝操作、析构且对象所有成员可移动，则编译器默认合成
	_task_queue->emplace(std::forward<task_type>(task));
#ifndef NDEBUG
	std::cout << "** (done) task queue size => " << _task_queue->size() << std::endl;
#endif
	// 解锁并唤醒阻塞在条件变量上的一个工作线程，让其处理任务
	lock.unlock();
	_cond_consumer.notify_one();
}

// 工作线程从请求队列中拉取任务进行处理
template <typename Callback>
void thread_pool<Callback>::worker() {
	// 从请求队列中拉取任务，需要对请求队列进行锁保护
	std::unique_lock<std::mutex> lock(_mutex);
	// 先判断线程池状态，若为开启则一直进行事件循环
	while (_pool_status != _pool_shutdown) {
		// 队列中有任务，工作线程拉取并处理
		if (!_task_queue->empty()) {
			// 这两种方式可以通过编译，运行也不会报错
			task_type task = _task_queue->front();
			/* auto task = std::move(_task_queue->front()); */

			// 以下两种方式可以通过编译，但是运行会报错--std::bad_function_call
			// 而且最操蛋的是如果在调用add_task时，添加延时操作，能成功运行
			// 但跑到一半会报错--std::bad_function_call，现在还想不出原因，待日后解决
			/* task_type&& task = std::move(_task_queue->front()); */
			/* const task_type& task = std::move(_task_queue->front()); */

#ifndef NDEBUG
			std::cout << "\nprocess task => " << &task << "..." << std::endl;
#endif
			_task_queue->pop();
			// 取出任务就解锁，并唤醒可能阻塞的生产者线程
			lock.unlock();
			_cond_producer.notify_one();
			// 工作线程执行任务
			task();
#ifndef NDEBUG
			std::cout << "** (done) worker thread => " << std::hex
				<< std::this_thread::get_id() << std::noshowbase << std::endl;
#endif
			// 再次加锁，用于下一次循环中拉取任务时，对请求队列进行锁保护
			lock.lock();
		}
		// 无任务则工作线程阻塞以待有任务处理
		else {
#ifndef NDEBUG
			std::cout << "\ntask queue is empty..." << std::endl;
			std::cout << "** (wait) worker thread => " << std::hex
				<< std::this_thread::get_id() << std::noshowbase << std::endl;
#endif
			// 若工作线程此时处于加锁状态，则自动解锁，但不解除阻塞
			// 若工作线程解除阻塞，则再次加锁，以执行临界区（下一次循环）
			_cond_consumer.wait(lock);
		}
	}
}

#endif
