# rng

splitmix64 计数器型随机流：快照、分叉、无偏范围，以及命名子流与并行分片。
`make test` 跑内置自检（含 ASan/UBSan 验证）。

## 基础能力

- `stream_init / stream_next` — 确定性序列，同种子同序列
- `stream_draw_at(base, k)` — 随机寻址：第 k 次抽取是 `(base, k)` 的纯函数
- 快照即结构体拷贝；`stream_fork(tag)` 按标签分叉
- `stream_range(n)` — 拒绝采样，无偏 `[0, n)`

## 命名子流

```c
NameRegistry reg; registry_init(&reg);
Stream combat, audio;
substream_create(&reg, &root, "combat", &combat);  /* FNV-1a 名字 → mix → 独立 base */
substream_create(&reg, &root, "audio",  &audio);
```

- 同一父流下名字不同 → base 不同；同名重复注册返回 `-2`（命名冲突检出），注册表不被污染
- 派生是纯函数：同名同父流永远得到同一子流，可复现
- 父流不同即使同名也得到不同子流；嵌套子流用全路径名（如 `"combat/hit"`）保持注册表扁平
- 派生 base 碰撞返回 `-3`，表满返回 `-4`，空名/超长名返回 `-1`

## 并行分片（leapfrog）

```c
Shard sh;
shard_open(&combat, /*shard_id=*/i, /*shard_count=*/n, &sh);
shard_next(&sh);   /* 全局下标 i, i+M, i+2M, ...，M = SHARD_MAX（固定上限 64） */
```

- **互不重叠**：各片是下标模 M 的不同剩余类，结构性不交，非概率保证
- **分布一致**：每次抽取严格等于 `stream_draw_at(base, 全局下标)`，分片只是单流取值的一个排列/子序列，统计分布不变
- **分片数变化可复现**：片 i 的序列只依赖 `i` 与固定上限 `M`，与实际片数 n 无关；n 从 2 改到 64，只要片 id 不变，其序列逐值相同
- 无块翻页：计数器无限跨步，不存在耗尽/换块问题

代价：n < M 时各片并集是 M 跨步子序列而非连续前缀（要连续前缀就得让步长随 n 变，会破坏 n 变化时的可复现性）。需要更大并行度时调大 `SHARD_MAX`。
