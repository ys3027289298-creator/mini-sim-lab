# pool

定容对象池（`POOL_CAP = 16`），32 位槽代际号，悬垂句柄检测。

`make test` 跑自检。

## 代际溢出回收

槽代际只增不减，回绕会让极旧的句柄重新有效。因此在代际逼近
`POOL_GEN_MAX` 时统一扫池（`POOL_GEN_MARGIN` 为余量）：

- 自动扫池只在池为空时发生（最后一次 `release` 之后）；扫池把每个槽的
  代际重置为 1、`epoch` 加一，此前签发的全部句柄作废。
- `pool_reset()` 强制扫池，即使仍有存活对象（这些对象被丢弃）。
- 每次扫池生成迁移表 `old_gen -> new_gen`（含新旧 epoch），见
  `pool_lookup_old()` / `pool_migration_count()` / `pool_migration_at()`。
- 句柄带 `epoch`，扫池前的旧句柄不可能与新签发句柄别名。
- 越过代际上限且无法扫池（仍有存活句柄）时，`release` 返回
  `POOL_ERR_GEN_LIMIT`、保持槽位不变，并写错误日志（stderr +
  `Pool.last_log`）。

旧句柄查询 `pool_lookup_old()` 总是给出明确原因，不会返回空值：

| 返回 | 含义 |
|---|---|
| `POOL_OK` | 句柄仍有效 |
| `POOL_ERR_FREED` | 槽已释放 |
| `POOL_ERR_GEN` | 当前 epoch 内代际不匹配 |
| `POOL_ERR_RESET` | 被最近一次扫池作废（迁移表可用） |
| `POOL_ERR_RESET_TOO_OLD` | 被更早的扫池作废 |
| `POOL_ERR_INDEX` | 索引越界 |

## 句柄与引用计数

`Handle{index, gen, epoch}`。`pool_alloc` 返回引用计数 1 的句柄；
`pool_retain` 加一；`pool_free` / `pool_release` 减一，归零才回收槽位并
推进代际。`pool_refcount` 查询计数。

## 查询

- `pool_slot_gen(index, &gen)`：当前槽代际。
- `pool_stats(PoolStats*)`：`used`、`high_water`、`epoch`、`resets`、
  `alloc_count`、`gens_retired`（已消耗代际数）、`max_gen_seen`、
  `log_count`。

## 对账自检

自检含一套逐次分配/释放的 64 位戳参考模型；随机 2000 步，每步之后
逐项比对两边全部句柄的有效性与引用计数、`used` 计数，并检查已释放句柄
始终无效。另覆盖接近上限扫池、迁移表内容、越过上限失败并记日志、
强制重置后复用等路径。
