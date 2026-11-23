# rng

`make test` 跑自检（输出与改造前逐字节一致）。

`make evidence` 构建确定性证据驱动，`./evidence --shared` 打印逐位哈希与
线程无关性对比表；完整复现见 `../det/README.md`。
