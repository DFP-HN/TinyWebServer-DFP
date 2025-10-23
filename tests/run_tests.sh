#!/bin/bash

# 测试运行脚本

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 切换到tests目录
cd "$(dirname "$0")"

# 显示帮助
show_help() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  all         - Build and run all tests"
    echo "  unit        - Run unit tests only"
    echo "  build       - Build all tests"
    echo "  clean       - Clean build artifacts"
    echo "  help        - Show this help message"
    echo ""
}

# 构建所有测试
build_tests() {
    echo -e "${YELLOW}Building all tests...${NC}"
    make -f Makefile.test all
    echo -e "${GREEN}Build complete!${NC}"
}

# 运行单元测试
run_unit_tests() {
    echo -e "${YELLOW}Running unit tests...${NC}"
    echo ""

    # Task测试
    if [ -f "unit/coroutine/task_test" ]; then
        echo "=== Task Tests ==="
        ./unit/coroutine/task_test
        echo ""
    fi

    # Scheduler测试
    if [ -f "unit/coroutine/scheduler_test" ]; then
        echo "=== Scheduler Tests ==="
        ./unit/coroutine/scheduler_test
        echo ""
    fi

    # ThreadPool测试
    if [ -f "unit/threadpool/cpu_thread_pool_test" ]; then
        echo "=== CpuThreadPool Tests ==="
        ./unit/threadpool/cpu_thread_pool_test
        echo ""
    fi

    # FileDBManager测试
    if [ -f "unit/http/file_db_manager_test" ]; then
        echo "=== FileDBManager Tests ==="
        ./unit/http/file_db_manager_test
        echo ""
    fi

    echo -e "${GREEN}Unit tests complete!${NC}"
}

# 清理
clean_tests() {
    echo -e "${YELLOW}Cleaning test artifacts...${NC}"
    make -f Makefile.test clean
    echo -e "${GREEN}Clean complete!${NC}"
}

# 主函数
main() {
    case "${1:-all}" in
        all)
            build_tests
            echo ""
            run_unit_tests
            ;;
        unit)
            run_unit_tests
            ;;
        build)
            build_tests
            ;;
        clean)
            clean_tests
            ;;
        help|--help|-h)
            show_help
            ;;
        *)
            echo -e "${RED}Unknown command: $1${NC}"
            echo ""
            show_help
            exit 1
            ;;
    esac
}

main "$@"
