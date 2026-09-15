# serial-data-gateway —— 零第三方依赖，只需要 g++ 与 pthread
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
BIN := serial-data-gateway

.PHONY: all clean run test

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.cpp src/gateway.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 无硬件冒烟测试：启动模拟源，用 nc 发命令，再优雅退出
run: $(BIN)
	./$(BIN) --simulate --listen 9000

test: $(BIN)
	bash tests/smoke_test.sh

clean:
	rm -f $(OBJ) $(BIN) records.bin
