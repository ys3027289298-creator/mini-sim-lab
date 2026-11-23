# pool

定容对象池、代际句柄、悬垂检测。

- 公开接口：`include/pool.h`（`Pool`、`Handle`、`POOL_CAP`、`pool_init/alloc/valid/get/free`）
- 实现：`src/pool.c`；自测：`src/selftest.c`（输出与单文件时代逐位一致）

`make test` 跑自检。
