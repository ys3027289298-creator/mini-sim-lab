# fixedstep

固定步长累积器、插值 alpha、追帧上限。

- 公开接口：`include/fixedstep.h`（`Clock`、`clock_init`、`clock_advance`）
- 实现：`src/fixedstep.c`；自测：`src/selftest.c`（输出与单文件时代逐位一致）

`make test` 跑自检。
