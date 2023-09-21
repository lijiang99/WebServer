objects = main.o http_connection.o log.o connection_pool.o
server : $(objects)
	g++ -o server $(objects) -lmysqlclient -std=c++20 -D NDEBUG

main.o : main.cpp
	g++ -c main.cpp -std=c++20 -D NDEBUG
http_connection.o : ./http/http_connection.cpp
	g++ -c ./http/http_connection.cpp -std=c++20 -D NDEBUG
log.o : ./log/log.cpp
	g++ -c ./log/log.cpp -std=c++20 -D NDEBUG
connection_pool.o : ./pool/connection_pool.cpp
	g++ -c ./pool/connection_pool.cpp -std=c++20 -D NDEBUG

.PHONY : clean
clean:
	-rm server $(objects) WebServer*.log
