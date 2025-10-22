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
       ./epoll/epoll_manager.cpp \
       ./user/user_manager.cpp \
       ./cache/static_cache.cpp \
       ./threadpool/work_stealing_pool.cpp

# 条件添加源文件
ifeq ($(USE_ZERO_COPY), 1)
    SRCS += http/zero_copy.cpp
endif

ifeq ($(USE_IO_URING), 1)
    SRCS += io_uring/io_uring_manager.cpp
endif

ifeq ($(USE_COROUTINE), 1)
    SRCS += http/streaming_multipart_parser.cpp
endif

server: $(SRCS)
	@echo "Building with refactored files: $(SRCS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	$(CXX) -o server $^ $(CXXFLAGS) $(LDFLAGS)

# 使用备份文件构建（用于 Docker）
# 注意：现在直接使用重构后的文件，不再从备份复制
server-from-backup:
	@echo "Building from refactored files (no backup needed)..."
	$(CXX) $(CXXFLAGS) -o server main.cpp config.cpp \
		webserver.cpp \
		http/http_conn.cpp \
		http/zero_copy.cpp \
		./timer/lst_timer.cpp \
		./log/log.cpp \
		./CGImysql/sql_connection_pool.cpp \
		./epoll/epoll_manager.cpp \
		./user/user_manager.cpp \
		./cache/static_cache.cpp \
		./threadpool/work_stealing_pool.cpp \
		-lpthread -lmysqlclient

clean:
	rm -f server

.PHONY: clean server-from-backup
