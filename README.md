# mini-sim-lab

游戏运行时基础设施，C 实现，每目录独立构建可自测。

| 目录 | 内容 |
|---|---|
| `fixedstep/` | 固定步长累积器（纳秒定点）、插值 alpha、追帧上限；`-DFIXEDSTEP_LEGACY` 可切回旧 double/fmod 实现 |
| `pool/` | 定容对象池、代际句柄、悬停检测 |
| `rng/` | 随机流、快照与分叉、无偏范围 |
| `sim/` | 回放仿真框架：`sim`（确定性构建）与 `sim-legacy`（改动前行为，复现漂移） |
| `tools/` | `reproduce.sh` 确定性矩阵（3 重复 × 5 线程数 × 2 分组）、`summarize.sh` 轨迹摘要与逐位对比 |

## 自测与确定性验证

```bash
make -C fixedstep test && make -C pool test && make -C rng test   # 各库 all_ok=1
./tools/reproduce.sh    # 30 次固定版运行逐字节一致，legacy 漂移；退出码 0=PASS
```

设计说明与验收数据见 [`artifacts/REPORT.md`](artifacts/REPORT.md)。
