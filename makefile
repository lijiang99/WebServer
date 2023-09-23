CXX := g++
CXXFLAGS := -std=c++20

TARGET := server
OBJS := main.o http_connection.o log.o connection_pool.o

DEBUGE := 0
ifeq ($(DEBUGE), 1)
	CXXFLAGS += -g -Wall
else
	CXXFLAGS += -O2 -D NDEBUG
endif

vpath %.h http:log:pool
vpath %.cpp http:log:pool

build: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) -lmysqlclient

$(OBJS): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY : clean
clean:
	-rm -f $(TARGET) $(OBJS) WebServer*.log
