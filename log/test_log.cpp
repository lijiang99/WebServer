#include <iostream>
#include <chrono>

#include "log.h"

int main() {
	auto logger = log::get_instance();
	logger->init("./", 50, 100);
	/* logger->write_log(log_level::info, "VARMILO", 726, 544.72); */
	/* logger->init("./", -50, 100); */
	/* logger->init("./", -50, -100); */
	/* logger->init("./", 50, 100); */
	/* std::cout << log::get_current_time() << std::endl; */
	for (int i = 0; i < 100; ++i) {
		LOG_DEBUG("Hello, World!", 726, 544.72, '\n');
		LOG_INFO("Hello, World!", 726, 544.72, '\n');
		LOG_WARN("Hello, World!", 726, 544.72, '\n');
		LOG_ERROR("Hello, World!", 726, 544.72, '\n');
	}
	std::this_thread::sleep_for(std::chrono::seconds(3));
}
