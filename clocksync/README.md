# clocksync

多时钟同步：物理低频固定 tick、渲染高频固定帧，两张网格都派生自同一条
int64 纳秒主时间线，而不是两个浮点钟互相对齐。

`make test` 跑自检，覆盖以下性质：

| 自检项 | 对应需求 |
|---|---|
| `ticks_for_*` / `frames_for_*` / `ticks_per_frame` | 按比率换算，整数热路径无浮点误差 |
| `equiv_independent_*` | 与各自独立推进的整数累积器再对齐的结果完全一致（含非零网格原点） |
| `equiv_through_rate_changes` | 比率变化后仍与独立钟等价 |
| `unwrapped_smooth` / `alpha_bounded_jitter` | 插值系数逐帧连续、始终落在 `[0,1)` |
| `hour_*_bounded` | 模拟一小时后两钟网格偏差严格小于 `P + R`，与运行时长无关 |
| `rate_change_*_no_jump` | 改物理/渲染周期后渲染时刻（vis time）不跳变 |
| `float32_loses_tick` / `double_longrun_loses_tick` | 对照组：独立浮点累积器一小时丢一个 tick（float32）、长跑同样丢 tick（double）；整数实现为 0 |
| `catchup_*` | 追帧上限语义与 `fixedstep/` 一致，仅状态瞬移、时间坐标不跳 |

## 设计要点

- tick/帧下标用整数除法从主钟反解：`idx = (t - origin) / period`，
  相位用取模：`phase = (t - origin) % period`，alpha = `phase / period`。
- 每个钟落后主钟不到一个自身周期，因此两钟偏差上界为
  `P + R`，跑多久都一样。
- 渲染时刻 `vis = origin + idx*P + phase` 恒等于主钟 `t`；改周期时把新
  网格的边界重锚到当前时刻（相位内的整 tick 立即排空，alpha 保持
  `[0,1)`），vis 不变，因此比率变化无跳变。
