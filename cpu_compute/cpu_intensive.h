#ifndef CPU_INTENSIVE_H
#define CPU_INTENSIVE_H

#include <string>

// CPU密集型计算类
class CPUIntensive {
public:
    // 计算质数 - level控制计算到第几个质数
    // level=1 -> 1000个质数, level=5 -> 5000个质数
    static std::string compute_primes(int level);

    // 斐波那契数列（递归版，CPU密集）
    // level=1 -> fib(35), level=5 -> fib(40)
    static std::string compute_fibonacci(int level);

    // 排序计算 - level控制数组大小
    // level=1 -> 50000元素, level=5 -> 250000元素
    static std::string compute_sort(int level);

    // 矩阵乘法
    // level=1 -> 200x200, level=5 -> 400x400
    static std::string compute_matrix(int level);

    // 综合计算（包含多种CPU密集操作）
    static std::string compute_mixed(int level);

private:
    // 辅助函数
    static bool is_prime(long long n);
    static long long fibonacci_recursive(int n);
};

#endif
