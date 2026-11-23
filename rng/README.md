# rng

splitmix64 风格随机流，支持快照、分叉、无偏范围。

- 公开接口：`include/rng.h`（`Stream`、`stream_init/next/range/fork`）
- 实现：`src/rng.c`；自测：`src/selftest.c`（输出与单文件时代逐位一致）

`make test` 跑自检。
