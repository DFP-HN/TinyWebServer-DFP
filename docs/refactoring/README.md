# 重构文档

本目录包含TinyWebServer重构相关的设计文档和实施记录。

## 📚 文档列表

### 📐 [REFACTORING.md](./REFACTORING.md) - 重构设计文档
> **核心设计文档**

**适合人群**: 想要了解重构设计思路的开发者

**内容**:
- 重构动机和目标
- 依赖注入设计
- EpollManager和UserManager架构
- 消除静态耦合的方案
- 设计模式应用

**阅读时间**: 30-40分钟

---

### ✅ [REFACTORING_COMPLETE.md](./REFACTORING_COMPLETE.md) - 重构完成报告

**适合人群**: 想快速了解重构成果的开发者

**内容**:
- 重构成果总结
- 改进点清单
- 测试验证结果
- 性能对比数据

**阅读时间**: 15-20分钟

---

### 📋 [IMPLEMENTATION_STATUS.md](./IMPLEMENTATION_STATUS.md) - 实施状态

**适合人群**: 需要了解重构进度的开发者

**内容**:
- 已完成功能清单
- 待优化事项
- 已知问题列表
- 未来计划

**使用场景**: 跟踪重构进度

---

## 🎯 重构核心改进

### 消除静态依赖

**重构前**:
```cpp
class http_conn {
    static int m_epollfd;      // ❌ 全局静态
    static int m_user_count;   // ❌ 全局静态
};
```

**重构后**:
```cpp
class http_conn {
    void set_epoll_manager(EpollManager *mgr);  // ✅ 依赖注入
    void set_user_manager(UserManager *mgr);    // ✅ 依赖注入
};
```

### 新增组件

- **EpollManager**: 封装所有epoll操作
- **UserManager**: 管理用户数据和连接计数
- **依赖注入**: 显式管理对象依赖关系

### 主要收益

✅ **可测试性**: 可以mock依赖进行单元测试
✅ **可维护性**: 依赖关系清晰，易于理解
✅ **可扩展性**: 支持多实例部署
✅ **线程安全**: 集中化的同步控制

---

## 🔗 推荐阅读路径

### 场景1: 我想了解为什么要重构

```
REFACTORING.md（前半部分：问题分析）
```

**预计时间**: 15分钟

---

### 场景2: 我想了解重构设计方案

```
REFACTORING.md → REFACTORING_COMPLETE.md
```

**预计时间**: 50分钟

---

### 场景3: 我想查看重构进度

```
IMPLEMENTATION_STATUS.md
```

**预计时间**: 5分钟

---

## 🔗 相关链接

- **主文档**: [返回根目录](../../README.md)
- **架构文档**: [CLAUDE.md](../../CLAUDE.md)
- **快速开始**: [docs/getting-started/](../getting-started/)
- **优化文档**: [docs/optimization/](../optimization/)

---

**最后更新**: 2025-10-18
