# 工作窃取线程池 - 构建说明

## 环境要求

- **操作系统**: Linux (Ubuntu 16.04+ 或类似发行版)
- **编译器**: g++ 支持 C++17 (g++ 7.0+)
- **依赖库**:
  - pthread (POSIX 线程库)
  - MySQL Client Library (libmysqlclient-dev)

## 安装依赖

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    libmysqlclient-dev \
    mysql-server
```

### CentOS/RHEL

```bash
sudo yum install -y \
    gcc-c++ \
    mysql-devel \
    mysql-server
```

## 编译项目

### 方法 1: 使用 make

```bash
# 清理旧构建
make clean

# 编译服务器
make server

# 检查编译结果
ls -lh server
```

### 方法 2: 使用 build.sh

```bash
sh ./build.sh
```

### 方法 3: 手动编译

```bash
g++ -o server \
    main.cpp \
    config.cpp \
    webserver.cpp \
    http/http_conn.cpp \
    timer/lst_timer.cpp \
    log/log.cpp \
    CGImysql/sql_connection_pool.cpp \
    epoll/epoll_manager.cpp \
    user/user_manager.cpp \
    cache/static_cache.cpp \
    threadpool/work_stealing_pool.cpp \
    -std=c++17 \
    -lpthread \
    -lmysqlclient \
    -O2
```

## 数据库配置

### 1. 创建数据库

```sql
CREATE DATABASE yourdb;
USE yourdb;

CREATE TABLE user (
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;

-- 添加测试用户
INSERT INTO user(username, passwd) VALUES('test', 'test123');
```

### 2. 修改数据库连接信息

编辑 `main.cpp` (第 6-8 行):

```cpp
string user = "root";           // MySQL 用户名
string passwd = "your_password"; // MySQL 密码
string databasename = "yourdb";  // 数据库名
```

## 运行服务器

### 基本运行

```bash
./server
```

默认配置:
- 端口: 9006
- 线程数: 8
- 日志: 同步模式
- Reactor 模式: Proactor

### 自定义参数

```bash
# 自定义端口和线程数
./server -p 9007 -t 16

# Reactor 模式 + 16 线程 + 异步日志
./server -a 1 -t 16 -l 1

# 完整参数示例
./server -p 9007 -l 1 -m 3 -o 1 -s 10 -t 16 -c 0 -a 1
```

**参数说明**:
- `-p PORT`: 端口号 (默认: 9006)
- `-t NUM`: 工作线程数 (默认: 8)
- `-l MODE`: 日志模式 0=同步, 1=异步 (默认: 0)
- `-m MODE`: 触发模式 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET (默认: 0)
- `-o LINGER`: 优雅关闭 0=关, 1=开 (默认: 0)
- `-s NUM`: 数据库连接池大小 (默认: 8)
- `-c LOG`: 日志开关 0=开, 1=关 (默认: 0)
- `-a MODEL`: 并发模型 0=Proactor, 1=Reactor (默认: 0)

### 访问服务器

在浏览器中访问:

```
http://localhost:9006
```

或使用 curl:

```bash
curl http://localhost:9006
```

## 测试工作窃取线程池

### 编译独立测试程序

```bash
g++ -std=c++17 -o test_ws test_work_stealing.cpp -lpthread
```

### 运行测试

```bash
./test_ws
```

**预期输出**:

```
工作窃取线程池核心组件测试
==============================

=== 测试 Chase-Lev Deque ===
Pushed 10 tasks, size: 10
Popped 10 tasks from bottom
Stolen task: id=100
✓ Chase-Lev Deque 测试通过

=== 测试通告板机制 ===
Thread 0 announced work, old_board=0
Thread 2 announced work, old_board=1
Current board: 5 (binary: 00000101)
Thread 0 revoked, board: 4
✓ 通告板机制测试通过

=== 测试并发窃取 ===
Queue 0 has 100 tasks
Queue 1 has 200 tasks
Queue 2 has 300 tasks
Queue 3 has 400 tasks
Total tasks created: 1000
Total tasks processed: 1000
Time: 0 ms
✓ 并发窃取测试通过（所有任务都被处理）

所有测试完成！
```

## 性能测试

### 使用 Webbench 压力测试

```bash
cd test_pressure/webbench-1.5

# 编译 webbench（如果需要）
make

# 10000 并发，持续 60 秒
./webbench -c 10000 -t 60 http://localhost:9006/

# 测试静态文件
./webbench -c 5000 -t 30 http://localhost:9006/picture.html
```

### 性能优化建议

1. **关闭日志** (生产环境测试):
   ```bash
   ./server -c 1 -t 16
   ```

2. **使用 ET 模式**:
   ```bash
   ./server -m 3 -t 16  # ET+ET 模式
   ```

3. **增加线程数** (根据 CPU 核心数):
   ```bash
   # 查看 CPU 核心数
   nproc

   # 设置线程数为核心数的 1-2 倍
   ./server -t $(nproc)
   ```

4. **调整系统参数**:
   ```bash
   # 增加文件描述符限制
   ulimit -n 65536

   # 查看当前限制
   ulimit -a
   ```

## Docker 部署

### 使用 Docker Compose（推荐）

```bash
# 启动所有服务（包括 MySQL）
docker-compose up -d

# 查看日志
docker-compose logs -f webserver

# 停止服务
docker-compose down
```

### 单独构建 Docker 镜像

```bash
# 构建镜像
docker build -t tinywebserver-ws .

# 运行容器
docker run -d -p 9006:9006 \
    -e DB_HOST=your_mysql_host \
    --name webserver \
    tinywebserver-ws
```

## 故障排查

### 编译错误

**错误**: `fatal error: mysql/mysql.h: No such file or directory`

**解决**:
```bash
sudo apt-get install libmysqlclient-dev
```

---

**错误**: `error: 'optional' is not a member of 'std'`

**解决**: 确保使用 C++17:
```bash
g++ --version  # 检查版本 >= 7.0
g++ -std=c++17 ...  # 使用 -std=c++17 标志
```

### 运行时错误

**错误**: `Can't connect to MySQL server`

**解决**:
1. 检查 MySQL 服务是否运行:
   ```bash
   sudo systemctl status mysql
   ```
2. 检查数据库连接信息 (main.cpp)
3. 检查防火墙规则

---

**错误**: `Address already in use`

**解决**: 端口被占用，更换端口:
```bash
./server -p 9007
```

或杀死占用进程:
```bash
lsof -i :9006
kill -9 <PID>
```

### 性能问题

**问题**: QPS 低于预期

**排查步骤**:
1. 关闭日志: `./server -c 1`
2. 增加线程数: `./server -t 16`
3. 检查 CPU 使用率: `top` 或 `htop`
4. 检查网络带宽: `iftop`
5. 检查文件描述符限制: `ulimit -n`

## 从原线程池切换

如果需要切换回原线程池进行对比测试:

### 1. 修改 webserver.h

```cpp
// 注释掉工作窃取线程池
//#include "./threadpool/work_stealing_pool.h"
#include "./threadpool/threadpool.h"

// 修改成员变量
std::unique_ptr<threadpool<http_conn>> m_pool;
//std::unique_ptr<WorkStealingPool<http_conn>> m_pool;
```

### 2. 修改 webserver.cpp

```cpp
// thread_pool() 函数中
m_pool = std::make_unique<threadpool<http_conn>>(m_actormodel, m_connPool, m_thread_num);
//m_pool = std::make_unique<WorkStealingPool<http_conn>>(m_actormodel, m_connPool, m_thread_num);
```

### 3. 修改 makefile

```makefile
# 注释掉工作窃取线程池
SRCS = main.cpp \
       config.cpp \
       webserver.cpp \
       ...
       # ./threadpool/work_stealing_pool.cpp
```

### 4. 重新编译

```bash
make clean
make server
```

## 相关文档

- **设计文档**: `WORK_STEALING_DESIGN.md`
- **项目说明**: `CLAUDE.md`
- **重构说明**: `REFACTORING.md`
- **Docker 部署**: `DOCKER.md`

## 联系与反馈

如有问题或建议，请查阅:
- 项目 README
- GitHub Issues (如果项目托管在 GitHub)
- 代码注释和文档

---

**最后更新**: 2025-10-18
**版本**: Work-Stealing Thread Pool v1.0
