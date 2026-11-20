# recon

跨模块对账层。把 fixedstep / pool / rng 的计数按帧记入同一张表，并与各模块
自己的计数逐项比对。

## 计量口径

| 项 | 含义 | 类型 |
|---|---|---|
| `fs.steps` | 固定步推进次数 | 每帧增量 |
| `fs.dropped` | 追帧上限丢弃的步数 | 每帧增量 |
| `pool.allocs` | 成功分配次数 | 每帧增量 |
| `pool.frees` | 成功释放次数 | 每帧增量 |
| `pool.failed` | 池满导致的分配失败次数 | 每帧增量 |
| `pool.used` | 帧末占用槽位数 | 帧末快照 |
| `rng.draws` | 原始取值次数 | 每帧增量 |
| `rng.ranges` | 范围取值调用次数 | 每帧增量 |

注意 `rng.draws` 统计的是底层 `stream_next` 调用次数，一次 `stream_range`
内部可能因拒绝采样调用多次；`rng.ranges` 单独统计范围取值调用。两者口径
不同，对账时不要混用。

每项都记两份：

- `recorded`：对账层自己观察事件累加；
- `expected`：模块在帧末上报自身累计计数器，对账层转成帧内增量。

两份逐格比较，不等即报 `frame=<n> item=<name>`。

## 运行

```
make test          # 跨模块对账 harness
make module-tests  # 三个模块在 -DRECON 下的自检
```

模块默认构建不受影响：未定义 `RECON` 时所有埋点编译为空，二进制与开启前逐
字节相同（已用 md5 校验）。

## 失败处理

- 单元格不一致：`frame=<n> item=<name> recorded=... expected=... delta=...`
- 行校验和 / magic / 帧号不连续：`error: frame=<n> row checksum mismatch` 等
- 缺项：`error: frame=<n> item=<name> missing checkpoint`
- 表不完整：`error: ledger incomplete: seal expects N frames, have M`
- `recon_load` 拒绝损坏或不完整的表（返回 -2 / -4 / -5）。
