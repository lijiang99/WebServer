#include <iostream>
#include <chrono>
#include <thread>
#include "timer.h"

void hello() {
	std::cout << "hello, world" << std::endl;
}

int main() {
	timer_heap<timer_type> heap;
	timer_type timer;
	timer.timeout_callback = hello;

	for (int i = 0; i < 5; ++i) {
		std::chrono::milliseconds interval = (i & 1 ? std::chrono::milliseconds(100) : std::chrono::milliseconds(500));
		timer.expire = std::chrono::high_resolution_clock::now() + interval;
		heap.push_timer(timer);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	for (int i = 0; i < 5; ++i) {
		heap.tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
}
