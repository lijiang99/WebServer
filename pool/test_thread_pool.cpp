#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include "thread_pool.h"

std::atomic<int> sum = 0;
/* std::mutex mtx; */

void test() {
	++sum;
	--sum;
	/* std::this_thread::sleep_for(std::chrono::microseconds(1)); */
	/* std::cout << "Hello, World!" << std::endl; */
	/* std::this_thread::sleep_for(std::chrono::seconds(3)); */
}

int main() {
	thread_pool<decltype(test)> tpool(8, 100000);
	for (std::size_t i = 0; i < 50000000; ++i) {
		tpool.add_task(test);
		/* if (i % 1000000) */
		/* 	std::this_thread::sleep_for(std::chrono::microseconds(1)); */
	}
	std::this_thread::sleep_for(std::chrono::seconds(5));
	std::cout << "sum: " << sum << std::endl;
	return 0;
}
