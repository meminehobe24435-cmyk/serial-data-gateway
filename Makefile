# serial-data-gateway —— 零第三方依赖，只需要 g++ 与 pthread
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
BIN := serial-data-gateway

.PHONY: all clean run test mqtt-test arm arm-static

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
	rm -rf build

# ============================================================ MQTT 模块
# 刻意做成**不依赖 POSIX**（不用 poll/socket/fsync），所以它既能随网关
# 在 Linux/ARM 上跑，也能在 Windows 上单独编译与测试；传输层走抽象接口，
# 真机换成 socket 实现、单测用内存实现。
build/mqtt_test: src/mqtt.cpp tests/test_mqtt.cpp src/mqtt.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc \
	    src/mqtt.cpp tests/test_mqtt.cpp -o $@

mqtt-test: build/mqtt_test
	./build/mqtt_test

# ============================================================ ARM 交叉编译
# 需要 g++-aarch64-linux-gnu（Ubuntu: sudo apt install g++-aarch64-linux-gnu）
CROSS    ?= aarch64-linux-gnu-
ARCHFLAG ?= -march=armv8-a

arm:
	$(MAKE) clean
	$(MAKE) CXX=$(CROSS)g++ CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread $(ARCHFLAG)" \
	        LDFLAGS="-pthread" all

# 静态链接 libstdc++/libgcc：部署到精简 rootfs 时省事（代价是体积）
arm-static:
	$(MAKE) clean
	$(MAKE) CXX=$(CROSS)g++ CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread $(ARCHFLAG)" \
	        LDFLAGS="-pthread -static-libstdc++ -static-libgcc" all
