#!/bin/bash

# 协程+线程池混合架构测试脚本

echo "======================================================"
echo "  协程+线程池混合架构测试"
echo "======================================================"
echo ""

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# 检查服务器是否运行
check_server() {
    if ! curl -s http://localhost:9006/ > /dev/null; then
        echo -e "${RED}错误: 服务器未运行在端口9006${NC}"
        echo "请先启动服务器: ./server -e 2"
        exit 1
    fi
}

# 测试1: I/O密集型请求（文件下载）
test_io_intensive() {
    echo -e "${YELLOW}[测试1] I/O密集型：文件下载${NC}"
    echo "发送10个并发下载请求..."

    for i in {1..10}; do
        curl -s -o /dev/null -w "Request $i: %{http_code} (time: %{time_total}s)\n" \
            http://localhost:9006/download/test.txt &
    done
    wait

    echo -e "${GREEN}✓ I/O测试完成${NC}"
    echo ""
}

# 测试2: CPU密集型请求（质数计算）
test_cpu_intensive() {
    echo -e "${YELLOW}[测试2] CPU密集型：质数计算${NC}"
    echo "发送5个CPU密集请求（level=3）..."

    for i in {1..5}; do
        echo "Request $i: 质数计算..."
        curl -s "http://localhost:9006/cpu_compute?task=primes&level=3" | jq -r '.last_prime' &
    done
    wait

    echo -e "${GREEN}✓ CPU测试完成${NC}"
    echo ""
}

# 测试3: 混合场景
test_mixed() {
    echo -e "${YELLOW}[测试3] 混合场景：并发I/O + CPU请求${NC}"
    echo "同时发送I/O和CPU请求..."

    # 5个I/O请求
    for i in {1..5}; do
        curl -s -o /dev/null http://localhost:9006/download/test.txt &
    done

    # 3个CPU请求
    for i in {1..3}; do
        curl -s "http://localhost:9006/cpu_compute?task=fibonacci&level=2" > /dev/null &
    done

    # 5个API请求
    for i in {1..5}; do
        curl -s http://localhost:9006/api/files > /dev/null &
    done

    wait

    echo -e "${GREEN}✓ 混合测试完成${NC}"
    echo ""
}

# 测试4: 性能基准测试
test_benchmark() {
    echo -e "${YELLOW}[测试4] 性能基准测试${NC}"

    echo "纯I/O QPS测试（10秒）..."
    ab -n 1000 -c 100 -t 10 http://localhost:9006/ 2>&1 | grep "Requests per second"

    echo ""
    echo "CPU任务QPS测试（10秒）..."
    ab -n 100 -c 10 -t 10 "http://localhost:9006/cpu_compute?task=primes&level=1" 2>&1 | grep "Requests per second"

    echo -e "${GREEN}✓ 基准测试完成${NC}"
    echo ""
}

# 测试5: 获取监控数据
test_monitor() {
    echo -e "${YELLOW}[测试5] 监控数据获取${NC}"

    # 假设我们添加了 /monitor API
    echo "获取系统监控数据..."
    # curl -s http://localhost:9006/monitor | jq '.'

    echo "（注：需要在服务器中添加 /monitor API端点）"
    echo -e "${GREEN}✓ 监控测试完成${NC}"
    echo ""
}

# 测试6: 压力测试
test_stress() {
    echo -e "${YELLOW}[测试6] 压力测试${NC}"
    echo "发送1000个并发请求..."

    ab -n 1000 -c 500 http://localhost:9006/ > /tmp/ab_results.txt 2>&1

    echo "结果统计:"
    grep "Requests per second" /tmp/ab_results.txt
    grep "Time per request" /tmp/ab_results.txt
    grep "Failed requests" /tmp/ab_results.txt

    echo -e "${GREEN}✓ 压力测试完成${NC}"
    echo ""
}

# 主菜单
main() {
    check_server

    echo "请选择测试项目:"
    echo "1) I/O密集型测试"
    echo "2) CPU密集型测试"
    echo "3) 混合场景测试"
    echo "4) 性能基准测试"
    echo "5) 监控数据测试"
    echo "6) 压力测试"
    echo "7) 运行所有测试"
    echo "0) 退出"
    echo ""
    read -p "请输入选项 [0-7]: " choice

    case $choice in
        1) test_io_intensive ;;
        2) test_cpu_intensive ;;
        3) test_mixed ;;
        4) test_benchmark ;;
        5) test_monitor ;;
        6) test_stress ;;
        7)
            test_io_intensive
            test_cpu_intensive
            test_mixed
            test_benchmark
            test_monitor
            test_stress
            ;;
        0) echo "退出"; exit 0 ;;
        *) echo -e "${RED}无效选项${NC}"; exit 1 ;;
    esac

    echo ""
    echo -e "${GREEN}======================================================"
    echo "  测试完成！"
    echo "======================================================${NC}"
}

# 运行主程序
main
