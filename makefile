# 包含配置文件
include config.mk

# 基础源文件列表
SRCS = main.cpp \
       config.cpp \
       webserver.cpp \
       http/http_conn.cpp \
       http/file_db_manager.cpp \
       ./timer/lst_timer.cpp \
       ./log/log.cpp \
       ./CGImysql/sql_connection_pool.cpp \
       ./user/user_manager.cpp \
       ./cache/static_cache.cpp

# 条件添加源文件
ifeq ($(USE_ZERO_COPY), 1)
    SRCS += http/zero_copy.cpp
endif

ifeq ($(USE_IO_URING), 1)
    SRCS += io_uring/io_uring_manager.cpp
endif

ifeq ($(USE_COROUTINE), 1)
    SRCS += http/streaming_multipart_parser.cpp \
            cpu_compute/cpu_intensive.cpp
endif

server: $(SRCS)
	@echo "Building with coroutine/io_uring architecture: $(SRCS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	$(CXX) -o server $^ $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f server

.PHONY: clean
