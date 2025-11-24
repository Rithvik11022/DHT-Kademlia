CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Iinclude -g
SRCS = src/main.cpp src/node.cpp src/network.cpp src/routing_table.cpp src/storage.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = bin/kademlia

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -rf bin/*.o bin/kademlia
