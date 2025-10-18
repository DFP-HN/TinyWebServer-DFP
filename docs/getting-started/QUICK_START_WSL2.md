# 🚀 WSL2 快速开始指南

## 三步快速安装

### 步骤 1: 运行安装脚本

```bash
cd /home/dfp/TinyWebServer-DFP
./install_wsl2_env.sh
```

安装脚本会：
- ✅ 安装 g++, make, git
- ✅ 安装 MySQL Server
- ✅ 创建数据库 `yourdb`
- ✅ 插入测试用户

### 步骤 2: 编译项目

```bash
make clean
make server
```

### 步骤 3: 运行服务器

```bash
./server -t 8
```

### 步骤 4: 测试

打开新终端：
```bash
curl http://localhost:9006
```

---

## 🔍 环境检查

随时运行环境检查脚本：

```bash
./check_env.sh
```

输出示例：
```
================================================
TinyWebServer 环境检查
================================================

系统信息:
----------------------------------------
OS: Ubuntu 20.04.6 LTS
内核: 5.15.167.4-microsoft-standard-WSL2
架构: x86_64

构建工具:
----------------------------------------
✓ C++ 编译器: g++ (Ubuntu 9.4.0-1ubuntu1~20.04.2) 9.4.0
✓ Make 工具: GNU Make 4.2.1
✓ Git: git version 2.25.1

MySQL:
----------------------------------------
✓ MySQL 客户端: mysql  Ver 8.0.39-0ubuntu0.20.04.1
✓ MySQL 开发库: 已安装
✓ MySQL 服务: 运行中

检查总结
================================================
通过: 15
失败: 0

✓ 环境检查通过！可以开始使��。
```

---

## 📖 常用命令

### MySQL 管理

```bash
# 启动（每次 WSL2 重启后需要）
sudo service mysql start

# 检查状态
sudo service mysql status

# 登录数据库
mysql -uroot -proot

# 查看用户表
mysql -uroot -proot -e "USE yourdb; SELECT * FROM user;"
```

### 编译和运行

```bash
# 清理并编译
make clean && make server

# 运行（8 线程）
./server -t 8

# 运行（16 线程 + 无日志）
./server -t 16 -c 1

# Reactor 模式（16 线程）
./server -a 1 -t 16
```

### 测试

```bash
# 简单测试
curl http://localhost:9006

# 登录测试
curl -X POST http://localhost:9006/login \
     -d "username=test&password=test123"

# 压力测试
cd test_pressure/webbench-1.5
make
./webbench -c 1000 -t 10 http://localhost:9006/
```

### 工作窃取线程池测试

```bash
# 编译测试程序
g++ -std=c++17 -o test_ws test_work_stealing.cpp -lpthread

# 运行测试
./test_ws

# 预期输出
工作窃取线程池核心组件测试
==============================

✓ Chase-Lev Deque 测试通过
✓ 通告板机制测试通过
✓ 并发窃取测试通过（所有任务都被处理）

所有测试完成！
```

---

## ⚙️ 配置选项

### 服务器参数

```bash
./server [选项]

-p PORT     端口号 (默认: 9006)
-t NUM      线程数 (默认: 8)
-l MODE     日志模式 0=同步, 1=异步
-m MODE     触发模式 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET
-o LINGER   优雅关闭 0=关, 1=开
-s NUM      数据库连接池大小 (默认: 8)
-c LOG      日志开关 0=开, 1=关
-a MODEL    并发模型 0=Proactor, 1=Reactor
```

### 推荐配置

```bash
# 开发环境（带日志）
./server -t 8 -l 1

# 生产环境（高性能）
./server -t $(nproc) -m 3 -c 1

# 压力测试（无日志 + ET 模式）
./server -t 16 -m 3 -c 1
```

---

## 🐛 常见问题

### 问题 1: MySQL 连接失败

```bash
# 检查 MySQL 状态
sudo service mysql status

# 启动 MySQL
sudo service mysql start

# 测试连接
mysql -uroot -proot -e "SELECT 1;"
```

### 问题 2: 端口被占用

```bash
# 查看占用进程
lsof -i :9006

# 杀死进程
kill -9 <PID>

# 或使用其他端口
./server -p 9007
```

### 问题 3: 编译错��

```bash
# 重新安装依赖
sudo apt-get install libmysqlclient-dev

# 清理并重新编译
make clean
make server
```

### 问题 4: WSL2 重启后服务不可用

每次 WSL2 重启后，MySQL 服务会停止，需要重新启动：

```bash
sudo service mysql start
```

**永久解决方案**：编辑 `~/.bashrc`，添加：
```bash
# 自动启动 MySQL
if ! sudo service mysql status > /dev/null 2>&1; then
    sudo service mysql start > /dev/null 2>&1
fi
```

---

## 📁 文件结构

```
TinyWebServer-DFP/
├── install_wsl2_env.sh          ← 自动安装脚本
├── check_env.sh                 ← 环境检查脚本
├── WSL2_SETUP_GUIDE.md          ← 详细安装指南
├── QUICK_START_WSL2.md          ← 本文件
├── WORK_STEALING_DESIGN.md      ← 工作窃取设计文档
├── BUILD_INSTRUCTIONS.md        ← 通用构建指南
├── main.cpp                     ← 主程序
├── webserver.cpp/h              ← Web 服务器核心
├── makefile                     ← 编译配置
├── threadpool/
│   ├── chase_lev_deque.h        ← Chase-Lev 无锁队列
│   ├── work_stealing_pool.h     ← 工作窃取线程池
│   └── work_stealing_pool.cpp   ← 全局变量定义
├── test_work_stealing.cpp       ← 独立测试程序
└── ...
```

---

## 🎯 性能调优

### 系统优化

```bash
# 增加文件描述符限制
ulimit -n 65536

# 检查 CPU 核心数
nproc

# 查看内存
free -h
```

### 应用优化

```bash
# 线程数 = CPU 核心数 × 1.5
./server -t $(($(nproc) * 3 / 2))

# 关闭日志（测试时）
./server -c 1

# 使用 ET 模式
./server -m 3
```

### MySQL 优化

```bash
sudo vim /etc/mysql/mysql.conf.d/mysqld.cnf
```

添加：
```ini
[mysqld]
max_connections = 1000
innodb_buffer_pool_size = 256M
```

重启 MySQL：
```bash
sudo service mysql restart
```

---

## 📊 性能测试

### Webbench 测试

```bash
cd test_pressure/webbench-1.5
make

# 轻量测试（1000 并发，10 秒）
./webbench -c 1000 -t 10 http://localhost:9006/

# 压力测试（10000 并发，60 秒）
./webbench -c 10000 -t 60 http://localhost:9006/
```

### 工作窃取线程池测试

```bash
# 测试核心组件
./test_ws

# 压力测试对比
# 1. 使用工作窃取池
./server -t 16 -c 1
./webbench -c 10000 -t 60 http://localhost:9006/

# 2. （如需对比原线程池，修改代码重新编译）
```

---

## 📚 相关文档

| 文档 | 说明 |
|-----|------|
| `QUICK_START_WSL2.md` | 本文件 - 快速开始 |
| `WSL2_SETUP_GUIDE.md` | 详细安装指南 |
| `WORK_STEALING_DESIGN.md` | 工作窃取设计文档 |
| `BUILD_INSTRUCTIONS.md` | 通用构建指南 |
| `WORK_STEALING_SUMMARY.md` | 实现总结 |
| `QUICK_REFERENCE.md` | 快速参考卡 |
| `CLAUDE.md` | 项目说明 |

---

## 🎓 学习路径

### 初学者

1. ✅ 运行 `./install_wsl2_env.sh`
2. ✅ 编译并运行 `make server && ./server`
3. ✅ 测试访问 `curl http://localhost:9006`
4. 📖 阅读 `CLAUDE.md` 了解项目架构

### 进阶

1. 📖 阅读 `WORK_STEALING_DESIGN.md` 了解工作窃取
2. 🔬 运行 `./test_ws` 理解核心算法
3. 📝 修改 `threadpool/work_stealing_pool.h` 自定义参数
4. 📊 压力测试对比性能

### 专家

1. 📖 阅读 Chase & Lev (2005) 论文
2. 🔬 实现内存回收（Hazard Pointer）
3. 🚀 添加 NUMA 感知调度
4. 📊 实现详细的性能统计

---

## ❓ 获取帮助

### 检查环境

```bash
./check_env.sh
```

### 查看日志

```bash
# 服务器日志（如果启用）
tail -f ServerLog

# MySQL 日志
sudo tail -f /var/log/mysql/error.log

# 系统日志
dmesg | tail
```

### 调试模式

```bash
# 编译调试版本
make clean
DEBUG=1 make server

# 使用 gdb
gdb ./server
(gdb) run -t 8
(gdb) bt  # 查看堆栈
```

---

## ✅ 检查清单

完成以下检查确保环境正确：

- [ ] 运行 `./install_wsl2_env.sh` 完成
- [ ] `./check_env.sh` 所有项通过
- [ ] MySQL 服务运行中
- [ ] 数据库 `yourdb` 存在
- [ ] `make server` 编译成功
- [ ] `./test_ws` 测试通过
- [ ] `./server -t 8` 启动成功
- [ ] `curl http://localhost:9006` 返回 HTML

---

## 🎉 下一步

环境安装完成后，你可以：

1. **学习架构**：阅读 `WORK_STEALING_DESIGN.md`
2. **性能测试**：运行压力测试对比性能
3. **修改代码**：自定义线程池参数
4. **贡献代码**：改进算法实现

---

**祝你使用愉快！** 🚀

如有问题，请查看 `WSL2_SETUP_GUIDE.md` 中的"常见问题排查"部分。
