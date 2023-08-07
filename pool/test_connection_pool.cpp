#include <iostream>
#include <memory>
#include <thread>
#include "connection_pool.h"

int main() {
	auto a = connection_pool::get_instance();
	std::cout << (a == nullptr ? "nullptr" : "not nullptr") << std::endl;
	a->init("localhost", "root", 3306, "$Li&&990503", "web_server", 8);
	/* a->init("localhost", "root", 3306, "$Li&&990503", "web_server", -8); */
	auto conn = a->get_connection();
	/* std::this_thread::sleep_for(std::chrono::seconds(3)); */
	a->put_connection(conn);
	a->put_connection(nullptr);
	/* a->init("localhost", "root", 3306, "$Li&&990503", "web_server", 8); */
	return 0;
}
