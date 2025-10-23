#include "cpu_intensive.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstdlib>
#include <cstring>

// 判断是否为质数
bool CPUIntensive::is_prime(long long n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (long long i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0)
            return false;
    }
    return true;
}

// 计算质数
std::string CPUIntensive::compute_primes(int level) {
    int target = level * 1000;  // level=1 -> 1000个质数
    if (target > 50000) target = 50000;  // 限制最大数量

    int count = 0;
    long long num = 2;
    long long last_prime = 0;

    while (count < target) {
        if (is_prime(num)) {
            last_prime = num;
            count++;
        }
        num++;
    }

    std::ostringstream oss;
    oss << "{\"task\":\"primes\",\"level\":" << level
        << ",\"count\":" << target
        << ",\"last_prime\":" << last_prime << "}";
    return oss.str();
}

// 斐波那契数列（递归，非常CPU密集）
long long CPUIntensive::fibonacci_recursive(int n) {
    if (n <= 1) return n;
    return fibonacci_recursive(n - 1) + fibonacci_recursive(n - 2);
}

std::string CPUIntensive::compute_fibonacci(int level) {
    int n = 30 + level * 2;  // level=1 -> fib(32), level=5 -> fib(40)
    if (n > 45) n = 45;  // 限制最大值，避免过长时间

    long long result = fibonacci_recursive(n);

    std::ostringstream oss;
    oss << "{\"task\":\"fibonacci\",\"level\":" << level
        << ",\"n\":" << n
        << ",\"result\":" << result << "}";
    return oss.str();
}

// 排序计算
std::string CPUIntensive::compute_sort(int level) {
    int size = level * 50000;  // level=1 -> 50000个元素
    if (size > 1000000) size = 1000000;  // 限制最大大小

    std::vector<int> arr(size);

    // 生成随机数组
    for (int i = 0; i < size; i++) {
        arr[i] = rand() % 1000000;
    }

    // 快速排序
    std::sort(arr.begin(), arr.end());

    std::ostringstream oss;
    oss << "{\"task\":\"sort\",\"level\":" << level
        << ",\"size\":" << size
        << ",\"first\":" << arr[0]
        << ",\"median\":" << arr[size/2]
        << ",\"last\":" << arr[size-1] << "}";
    return oss.str();
}

// 矩阵乘法
std::string CPUIntensive::compute_matrix(int level) {
    int size = 100 + level * 50;  // level=1 -> 150x150, level=5 -> 350x350
    if (size > 500) size = 500;  // 限制最大大小

    std::vector<std::vector<double>> A(size, std::vector<double>(size));
    std::vector<std::vector<double>> B(size, std::vector<double>(size));
    std::vector<std::vector<double>> C(size, std::vector<double>(size, 0));

    // 初始化随机矩阵
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            A[i][j] = (double)rand() / RAND_MAX;
            B[i][j] = (double)rand() / RAND_MAX;
        }
    }

    // 矩阵乘法 O(n^3)
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            for (int k = 0; k < size; k++) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }

    std::ostringstream oss;
    oss << "{\"task\":\"matrix\",\"level\":" << level
        << ",\"size\":" << size << "x" << size
        << ",\"result_sample\":" << C[0][0] << "}";
    return oss.str();
}

// 综合计算
std::string CPUIntensive::compute_mixed(int level) {
    // 执行多种计算
    std::ostringstream oss;
    oss << "[";

    // 质数计算（较小level）
    oss << compute_primes(level < 5 ? level : level/2);
    oss << ",";

    // 斐波那契（小level）
    oss << compute_fibonacci(level < 3 ? level : level/2);
    oss << ",";

    // 排序
    oss << compute_sort(level);

    oss << "]";
    return oss.str();
}
