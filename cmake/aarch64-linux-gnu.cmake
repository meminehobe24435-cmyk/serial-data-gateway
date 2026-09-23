# aarch64 交叉编译工具链文件
#
# 用法（在 aarch64 Linux 板子上跑，比如 ARM 网关/工控机/Jetson）：
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
#   cmake --build build-arm -j
#
# 需要先装交叉工具链：
#   Ubuntu/Debian: sudo apt install g++-aarch64-linux-gnu
#   （若目标是 32 位 ARM：g++-arm-linux-gnueabihf，把下面的前缀改掉）
#
# 为什么要显式写这个文件而不是让用户自己设环境变量：
#   交叉编译最常踩的坑是「**误用了宿主机的头文件/库**」——
#   编出来的二进制在板子上跑不起来（GLIBC 版本不匹配 / 链接到 x86 的 .so）。
#   工具链文件把这三件事一次说清：编译器前缀、sysroot、以及禁止搜宿主机路径。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---- 交叉编译器 ----
set(TOOLCHAIN_PREFIX aarch64-linux-gnu)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)

# ---- sysroot（按实际工具链位置调整；不设则用编译器自带的） ----
# set(CMAKE_SYSROOT /usr/${TOOLCHAIN_PREFIX})

# ---- 严格模式：只在 sysroot 里找头文件与库，**不要**混进宿主机的 ----
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- 目标板 CPU 特性 ----
# 多数 ARM64 板子是 ARMv8-A；按需打开指令集优化（注意确认板子支持）
set(ARCH_FLAGS "-march=armv8-a")
set(CMAKE_C_FLAGS_INIT   "${ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${ARCH_FLAGS}")

# ---- 静态链接选项（部署到精简 rootfs 时常用） ----
# 打开后二进制不依赖目标板的 libstdc++，代价是体积变大：
#   -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
option(MQTT_STATIC_LINKOFF "静态链接 libstdc++/libgcc（便于部署到精简 rootfs）" OFF)
if(MQTT_STATIC_LINKOFF)
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
endif()
