CXX ?= g++

# C++17 标准（工作窃取线程池需要 std::optional）
CXXFLAGS += -std=c++17

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

# 重构后的源文件列表 - 包含所有已重构的文件
SRCS = main.cpp \
       config.cpp \
       webserver.cpp \
       http/http_conn.cpp \
       ./timer/lst_timer.cpp \
       ./log/log.cpp \
       ./CGImysql/sql_connection_pool.cpp \
       ./epoll/epoll_manager.cpp \
       ./user/user_manager.cpp \
       ./cache/static_cache.cpp \
       ./threadpool/work_stealing_pool.cpp

server: $(SRCS)
	@echo "Building with refactored files: $(SRCS)"
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient

# 使用备份文件构建（用于 Docker）
# 注意：现在直接使用重构后的文件，不再从备份复制
server-from-backup:
	@echo "Building from refactored files (no backup needed)..."
	$(CXX) $(CXXFLAGS) -o server main.cpp config.cpp \
		webserver.cpp \
		http/http_conn.cpp \
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
