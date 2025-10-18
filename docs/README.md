# TinyWebServer 文档中心

欢迎来到TinyWebServer文档中心！所有项目文档已按主题分类整理。

---

## 📂 文档分类

### 🚀 [快速开始 (getting-started/)](./getting-started/)

**适合新手和快速部署的文档**

| 文档 | 说明 | 时长 |
|------|------|------|
| [QUICK_START_WSL2.md](./getting-started/QUICK_START_WSL2.md) | WSL2环境快速开始 | 15分钟 |
| [QUICK_REFERENCE.md](./getting-started/QUICK_REFERENCE.md) | 快速参考手册 | 查阅型 |
| [BUILD_INSTRUCTIONS.md](./getting-started/BUILD_INSTRUCTIONS.md) | 构建指南 | 10分钟 |
| [WSL2_SETUP_GUIDE.md](./getting-started/WSL2_SETUP_GUIDE.md) | WSL2环境配置 | 30分钟 |

**推荐入口**: 新手从 `QUICK_START_WSL2.md` 开始

---

### 🔧 [重构文档 (refactoring/)](./refactoring/)

**架构重构设计和实施文档**

| 文档 | 说明 | 时长 |
|------|------|------|
| [REFACTORING.md](./refactoring/REFACTORING.md) | 重构设计文档 | 40分钟 |
| [REFACTORING_COMPLETE.md](./refactoring/REFACTORING_COMPLETE.md) | 重构完成报告 | 20分钟 |
| [IMPLEMENTATION_STATUS.md](./refactoring/IMPLEMENTATION_STATUS.md) | 实施状态 | 5分钟 |

**核心改进**:
- ✅ 依赖注入替代静态耦合
- ✅ EpollManager和UserManager架构
- ✅ 可测试、可维护、可扩展

---

### ⚡ [性能优化 (optimization/)](./optimization/)

**各项性能优化的设计和测试文档**

| 文档 | 说明 | 时长 |
|------|------|------|
| [OPTIMIZATIONS_README.md](./optimization/OPTIMIZATIONS_README.md) | 优化总览（**推荐**） | 40分钟 |
| [OPTIMIZATION_QUICKSTART.md](./optimization/OPTIMIZATION_QUICKSTART.md) | 快速开始 | 15分钟 |
| [OPTIMIZATION_PATCHES.md](./optimization/OPTIMIZATION_PATCHES.md) | 优化补丁集 | 查阅型 |
| [OPTIMIZATION_REPORT.md](./optimization/OPTIMIZATION_REPORT.md) | 优化报告 | 25分钟 |
| [SCENARIO_OPTIMIZATIONS.md](./optimization/SCENARIO_OPTIMIZATIONS.md) | 场景化优化 | 30分钟 |

**性能提升**:
- 📈 小文件QPS: 97K → 156K (**+60%**)
- 📈 混合负载: 65K → 103K (**+58%**)

---

### 🔄 [工作窃取线程池 (work-stealing/)](./work-stealing/)

**工作窃取线程池Bug修复和设计文档**

| 文档 | 说明 | 时长 |
|------|------|------|
| [WORK_STEALING_README.md](./work-stealing/WORK_STEALING_README.md) | 入口文档（**推荐**） | 5分钟 |
| [WORK_STEALING_INDEX.md](./work-stealing/WORK_STEALING_INDEX.md) | 完整索引 | 5分钟 |
| [WORK_STEALING_QUICK_FIX.md](./work-stealing/WORK_STEALING_QUICK_FIX.md) | 快速修复 | 10分钟 |
| [WORK_STEALING_BUG_FIX.md](./work-stealing/WORK_STEALING_BUG_FIX.md) | 完整分析 | 40分钟 |
| [WORK_STEALING_CODE_DIFF.md](./work-stealing/WORK_STEALING_CODE_DIFF.md) | 代码对比 | 20分钟 |
| [WORK_STEALING_DESIGN.md](./work-stealing/WORK_STEALING_DESIGN.md) | 设计原理 | 30分钟 |
| [WORK_STEALING_SUMMARY.md](./work-stealing/WORK_STEALING_SUMMARY.md) | 性能总结 | 20分钟 |

**修复成果**:
- ✅ 修复3个并发Bug（死循环、Lost Wakeup、唤醒失败）
- ✅ 性能提升68% vs 原始threadpool
- ✅ 10000并发零失败

---

## 🎯 快速导航

### 我是新手，想快速运行服务器

👉 [getting-started/QUICK_START_WSL2.md](./getting-started/QUICK_START_WSL2.md)

**预计时间**: 15分钟

---

### 我想了解项目架构

👉 [../CLAUDE.md](../CLAUDE.md) + [refactoring/REFACTORING.md](./refactoring/REFACTORING.md)

**预计时间**: 1小时

---

### 我想提升服务器性能

👉 [optimization/OPTIMIZATION_QUICKSTART.md](./optimization/OPTIMIZATION_QUICKSTART.md)

**预计时间**: 15分钟了解，30分钟应用

---

### 我遇到了工作窃取线程池的问题

👉 [work-stealing/WORK_STEALING_QUICK_FIX.md](./work-stealing/WORK_STEALING_QUICK_FIX.md)

**预计时间**: 10分钟修复

---

### 我想深入学习并发编程

👉 [work-stealing/WORK_STEALING_BUG_FIX.md](./work-stealing/WORK_STEALING_BUG_FIX.md)

**预计时间**: 2小时（完整阅读所有工作窃取文档）

---

## 📊 文档统计

| 分类 | 文档数 | 总大小 |
|------|--------|--------|
| 快速开始 | 4 | ~20KB |
| 重构文档 | 3 | ~25KB |
| 性能优化 | 5 | ~45KB |
| 工作窃取 | 7 | ~90KB |
| **总计** | **19** | **~180KB** |

---

## 🔗 根目录文档

这些重要文档保留在项目根目录：

- **[README.md](../README.md)**: 项目主README
- **[CLAUDE.md](../CLAUDE.md)**: 项目架构和使用指南

---

## 📖 推荐学习路径

### 路径1: 快速上手（30分钟）

```
1. getting-started/QUICK_START_WSL2.md
2. getting-started/QUICK_REFERENCE.md（需要时查阅）
```

---

### 路径2: 深入理解（3小时）

```
1. getting-started/QUICK_START_WSL2.md      （15分钟）
2. ../CLAUDE.md                              （20分钟）
3. refactoring/REFACTORING.md                （40分钟）
4. optimization/OPTIMIZATIONS_README.md      （40分钟）
5. work-stealing/WORK_STEALING_README.md     （5分钟）
6. work-stealing/WORK_STEALING_BUG_FIX.md    （40分钟）
```

---

### 路径3: 性能优化专家（2小时）

```
1. optimization/OPTIMIZATION_QUICKSTART.md   （15分钟）
2. optimization/OPTIMIZATIONS_README.md      （40分钟）
3. optimization/SCENARIO_OPTIMIZATIONS.md    （30分钟）
4. optimization/OPTIMIZATION_REPORT.md       （25分钟）
5. work-stealing/WORK_STEALING_SUMMARY.md    （20分钟）
```

---

## 🛠️ 文档维护

### 添加新文档

1. 确定文档分类
2. 放入对应目录
3. 更新该目录的README.md
4. 更新本索引文档

### 文档命名规范

- **快速开始**: `QUICK_START_*.md`, `*_GUIDE.md`
- **重构**: `REFACTORING*.md`, `IMPLEMENTATION*.md`
- **优化**: `OPTIMIZATION*.md`, `SCENARIO*.md`
- **工作窃取**: `WORK_STEALING_*.md`

---

## 📞 获取帮助

### 常见问题

1. **Q: 文档在哪里？**
   A: 按主题分类在docs/目录下，查看上面的分类

2. **Q: 我该从哪个文档开始？**
   A: 查看"快速导航"部分，根据你的需求选择

3. **Q: 找不到某个主题？**
   A: 查看各分类目录的README.md索引

### 联系方式

如有问题，请查看各文档或提交Issue。

---

**最后更新**: 2025-10-18
**文档总数**: 19份
**维护状态**: ✅ 活跃维护中
