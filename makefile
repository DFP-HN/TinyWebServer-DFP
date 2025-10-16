CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

# 重构后的源文件列表
SRCS = main.cpp \
       ./timer/lst_timer.cpp \
       ./http/http_conn.cpp \
       ./log/log.cpp \
       ./CGImysql/sql_connection_pool.cpp \
       webserver.cpp \
       config.cpp \
       ./epoll/epoll_manager.cpp \
       ./user/user_manager.cpp

server: $(SRCS)
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm -f server
