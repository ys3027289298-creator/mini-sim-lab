# mini-sim-lab

游戏运行时基础设施，C 实现，每目录独立构建可自测。

| 目录 | 内容 |
|---|---|
| `fixedstep/` | 固定步长累积器、插值 alpha、追帧上限 |
| `pool/` | 定容对象池、代际句柄、悬垂检测 |
| `rng/` | 随机流、快照与分叉、无偏范围 |
| `simlab/` | 三模块集成：分片 + 多线程，输出与线程数无关 |

每个库模块分为 `include/<mod>.h`（公开接口）、`src/<mod>.c`（实现）、`src/selftest.c`（自测）。

行为保持证据与一键复现：`./verify.sh`，报告见 `artifacts/evidence.md`。
