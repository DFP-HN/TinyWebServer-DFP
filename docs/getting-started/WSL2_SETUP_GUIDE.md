# WSL2 环境安装指南 - TinyWebServer

## 方法一：自动安装（推荐）

### 运行安装脚本

```bash
cd /home/dfp/TinyWebServer-DFP
chmod +x install_wsl2_env.sh
./install_wsl2_env.sh
```

安装脚本会自动完���：
1. ✅ 更新系统包
2. ✅ 安装构建工具（g++, make, git）
3. ✅ 安装 MySQL 服务器和开发库
4. ✅ 启动 MySQL 服务
5. ✅ 创建数据库和测试用户
6. ✅ 配置项目

---

## 方法二：手动安装

### 第一步：更新系统

```bash
sudo apt-get update
sudo apt-get upgrade -y
```

### 第二步：安装构建工具

```bash
sudo apt-get install -y build-essential g++ make git wget curl
```

验证安装：
```bash
g++ --version  # 应该显示 9.4.0 或更高
make --version
```

### 第三步：安装 MySQL

```bash
# 安装 MySQL 服务器和开发库
sudo apt-get install -y mysql-server mysql-client libmysqlclient-dev

# 启动 MySQL 服务
sudo service mysql start

# 检查状态
sudo service mysql status
```

### 第四步：配置 MySQL

#### 4.1 设置 root 密码（如果需要）

```bash
sudo mysql
```

在 MySQL 提示符下执行：
```sql
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'root';
FLUSH PRIVILEGES;
EXIT;
```

#### 4.2 创建数据库和表

```bash
mysql -uroot -proot
```

执行以下 SQL：
```sql
-- 创建数据库
CREATE DATABASE yourdb;

-- 使用数据库
USE yourdb;

-- 创建用户表
CREATE TABLE user(
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;

-- 插入测试数据
INSERT INTO user(username, passwd) VALUES('test', 'test123');
INSERT INTO user(username, passwd) VALUES('admin', 'admin');
INSERT INTO user(username, passwd) VALUES('guest', 'guest');

-- 验证数据
SELECT * FROM user;

-- 退出
EXIT;
```

### 第五步：配置项目

编辑 `main.cpp`（第 6-8 行）：
```cpp
string user = "root";           // MySQL 用户名
string passwd = "root";         // MySQL 密码
string databasename = "yourdb"; // 数据库名
```

### 第六步：编译项目

```bash
cd /home/dfp/TinyWebServer-DFP

# 清理旧文件
make clean

# 编译服务器
make server

# 检查编译结果
ls -lh server
```

### 第七步：运行服务器

```bash
# 基本运行（8 线程）
./server -t 8

# 或使用工作窃取线程池（16 线程 + 无日志）
./server -t 16 -c 1
```

### 第八步：测试

在另一个终端或浏览器中：
```bash
# 测试主页
curl http://localhost:9006

# 测试登录
curl -X POST http://localhost:9006/login \
     -d "username=test&password=test123"
```

---

## WSL2 特别注意事项

### 1. MySQL 服务不会自动启动

每次重启 WSL2 后需要手动启动 MySQL：
```bash
sudo service mysql start
```

**解决方案**：创建启动脚本

编辑 `~/.bashrc`，添加：
```bash
# 自动启动 MySQL
if ! sudo service mysql status > /dev/null 2>&1; then
    sudo service mysql start > /dev/null 2>&1
fi
```

### 2. 端口访问

从 Windows 访问 WSL2 服务：
```
http://localhost:9006
```

或使用 WSL2 IP 地址：
```bash
# 查看 WSL2 IP
ip addr show eth0 | grep inet

# 从 Windows 访问
http://<WSL2_IP>:9006
```

### 3. 防火墙设置

如果无法访问，检查 Windows 防火墙：
```powershell
# 在 Windows PowerShell (管理员) 中运行
New-NetFirewallRule -DisplayName "WSL2 WebServer" -Direction Inbound -LocalPort 9006 -Protocol TCP -Action Allow
```

### 4. 文件权限

确保脚本有执行权限：
```bash
chmod +x install_wsl2_env.sh
chmod +x build.sh
```

---

## 常见问题排查

### 问题 1: MySQL 连接失败

**错误信息**:
```
Can't connect to MySQL server on 'localhost'
```

**解决方案**:
```bash
# 1. 检查 MySQL 是否运行
sudo service mysql status

# 2. 如果未运行，启动它
sudo service mysql start

# 3. 测试连接
mysql -uroot -proot -e "SELECT 1;"
```

### 问题 2: 找不到 mysql.h

**错误信息**:
```
fatal error: mysql/mysql.h: No such file or directory
```

**解决方案**:
```bash
sudo apt-get install libmysqlclient-dev
```

### 问题 3: 编译错误 - optional not found

**错误信息**:
```
error: 'optional' is not a member of 'std'
```

**解决方案**:
确保使用 C++17：
```bash
# 检查 g++ 版本（需要 >= 7.0）
g++ --version

# 如果版本过低，升级
sudo apt-get install g++-9
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-9 90
```

### 问题 4: 端口已被占用

**错误信息**:
```
Address already in use
```

**解决方案**:
```bash
# 查找占用端口的进程
lsof -i :9006
# 或
netstat -tulnp | grep 9006

# 杀死进程
kill -9 <PID>

# 或使用其他端口
./server -p 9007
```

### 问题 5: 权限不��

**错误信息**:
```
Permission denied
```

**解决方案**:
```bash
# 修复文件权限
chmod +x server
chmod +x *.sh

# 修复目录权限
chmod -R 755 /home/dfp/TinyWebServer-DFP
```

---

## 快速命令参考

### MySQL 管理

```bash
# 启动
sudo service mysql start

# 停止
sudo service mysql stop

# 重启
sudo service mysql restart

# 状态
sudo service mysql status

# 登录
mysql -uroot -proot

# 快速查询
mysql -uroot -proot -e "USE yourdb; SELECT * FROM user;"
```

### 项目编译

```bash
# 清理
make clean

# 编译
make server

# 编译并运行
make server && ./server -t 8
```

### 服务器运行

```bash
# 默认配置（8 线程）
./server

# 自定义线程数
./server -t 16

# Reactor 模式 + 异步日志
./server -a 1 -l 1 -t 16

# 高性能配置（关闭日志）
./server -t 16 -m 3 -c 1

# 查看所有选项
./server -h
```

### 测试

```bash
# 编译测试工具
cd test_pressure/webbench-1.5
make

# 压力测试（1000 并发，10 秒）
./webbench -c 1000 -t 10 http://localhost:9006/

# 轻量测试
curl http://localhost:9006

# POST 测试
curl -X POST http://localhost:9006/login -d "username=test&password=test123"
```

---

## 性能优化建议

### 系统层面

```bash
# 增加文件描述符限制
ulimit -n 65536

# 检查限制
ulimit -a
```

### 应用层面

```bash
# 使用工作窃取线程池（已集成）
./server -t $(nproc)  # 线程数 = CPU 核心数

# 关闭日志（测试时）
./server -c 1 -t 16

# ET 模式（更高性能）
./server -m 3 -t 16
```

### MySQL 优化

编辑 `/etc/mysql/mysql.conf.d/mysqld.cnf`：
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

## 开发工作流

### 日常开发

```bash
# 1. 启动 MySQL（每次 WSL2 重启后）
sudo service mysql start

# 2. 修改代码
vim webserver.cpp

# 3. 重新编译
make clean && make server

# 4. 运行测试
./server -t 8

# 5. 另一个终端测试
curl http://localhost:9006
```

### 调试

```bash
# 编译调试版本
make clean
DEBUG=1 make server

# 使用 gdb 调试
gdb ./server
(gdb) run -t 8
```

### 性能分析

```bash
# 使用 perf（需要安装）
sudo apt-get install linux-tools-generic
perf record -g ./server -t 16
perf report

# 查看线程
ps -eLf | grep server

# 查看 CPU 使用率
top -H -p $(pgrep server)
```

---

## 卸载

如果需要卸载环境：

```bash
# 停止服务器
pkill -9 server

# 停止 MySQL
sudo service mysql stop

# 删除 MySQL
sudo apt-get purge mysql-server mysql-client mysql-common
sudo apt-get autoremove
sudo rm -rf /var/lib/mysql
sudo rm -rf /etc/mysql

# 删除项目
cd ~
rm -rf /home/dfp/TinyWebServer-DFP
```

---

## 总结

安装步骤：
1. ✅ 运行 `./install_wsl2_env.sh`（自动）
2. ✅ 或按手动步骤逐步安装
3. ✅ 编译项目 `make server`
4. ✅ 运行服务器 `./server -t 8`
5. ✅ 测试访问 `curl http://localhost:9006`

如遇问题，参考"常见问题排查"部分。

**下一步**: 运行安装脚本并编译项目！
