server: main.cpp ./pool/thread_pool.h ./http/http_connection.cpp ./http/http_connection.h ./log/log.cpp ./log/log.h ./pool/connection_pool.cpp ./pool/connection_pool.h
	g++ -o server main.cpp ./pool/thread_pool.h ./http/http_connection.cpp ./http/http_connection.h ./log/log.cpp ./log/log.h ./pool/connection_pool.cpp ./pool/connection_pool.h -lpthread -lmysqlclient -std=c++20 -D NDEBUG


clean:
	rm  -r server
