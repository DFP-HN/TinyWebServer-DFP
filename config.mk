# TinyWebServer 编译配置

# 是否启用 io_uring 支持（需要 Linux 5.1+ 和 liburing）
# 设置为 1 启用，0 禁用
USE_IO_URING ?= 1

# 是否启用 C++20 协程支持（需要 GCC 10+ 或 Clang 10+）
# 设置为 1 启用，0 禁用
USE_COROUTINE ?= 1

# 是否启用零拷贝优化（sendfile/splice）
# 设置为 1 启用，0 禁用
USE_ZERO_COPY ?= 1

# 编译器选择
CXX ?= g++-10

# 调试模式
DEBUG ?= 1

# 基础编译标志
CXXFLAGS = -std=c++17 -Wall -Wextra

ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -O0
else
    CXXFLAGS += -O2 -DNDEBUG
endif

# 链接库
LDFLAGS = -lpthread -lmysqlclient

# 条件编译选项

ifeq ($(USE_IO_URING), 1)
    CXXFLAGS += -DUSE_IO_URING
    LDFLAGS += -luring
    $(info [INFO] io_uring support enabled)
else
    $(info [INFO] io_uring support disabled)
endif

ifeq ($(USE_COROUTINE), 1)
    CXXFLAGS += -DUSE_COROUTINE -std=c++2a -fcoroutines
    LDFLAGS += -lssl -lcrypto
    $(info [INFO] C++20 coroutine support enabled)
else
    $(info [INFO] C++20 coroutine support disabled)
endif

ifeq ($(USE_ZERO_COPY), 1)
    CXXFLAGS += -DUSE_ZERO_COPY
    $(info [INFO] Zero-copy optimization enabled)
else
    $(info [INFO] Zero-copy optimization disabled)
endif

# 检查 liburing 是否可用
ifeq ($(USE_IO_URING), 1)
    LIBURING_CHECK := $(shell pkg-config --exists liburing && echo yes || echo no)
    ifneq ($(LIBURING_CHECK), yes)
        $(warning [WARNING] liburing not found! Please install liburing-dev or liburing-devel)
        $(warning [WARNING] On Ubuntu/Debian: sudo apt-get install liburing-dev)
        $(warning [WARNING] On CentOS/RHEL: sudo yum install liburing-devel)
    endif
endif
