#include <iostream>
#include <chrono>
#include <thread>
#include "timer.h"

void hello(client_data *data) {
	std::cout << "hello, world" << std::endl;
}

int main() {
	timer_heap<util_timer> heap;
	util_timer *timer = nullptr;
	util_timer *tmp_timer1 = nullptr;
	util_timer *tmp_timer2 = nullptr;

	for (int i = 0; i < 5; ++i) {
		timer = new util_timer();
		if (i == 1) tmp_timer1 = timer;
		else if (i == 2) tmp_timer2 = timer;
		std::chrono::milliseconds interval = (i & 1 ? std::chrono::milliseconds(100) : std::chrono::milliseconds(500));
		timer->expire = std::chrono::high_resolution_clock::now() + interval;
		timer->timeout_callback = hello;
		heap.push_timer(timer);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	tmp_timer1->expire += std::chrono::milliseconds(600);
	heap.adjust_timer(tmp_timer1);

	heap.del_timer(tmp_timer2);
	tmp_timer2 = nullptr;

	for (int i = 0; i < 5; ++i) {
		heap.tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
}
