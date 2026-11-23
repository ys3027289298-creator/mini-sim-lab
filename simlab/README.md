# simlab

三模块的连带集成：fixedstep 驱动 tick、pool 存实体、rng 按分片 `stream_fork`。

工作划分为固定的 8 个分片，每分片持有私有的 `Clock`/`Pool`/`Stream`；线程只负责执行分片，
结果按分片序号合并，因此完整输出与线程数和调度无关（逐位一致，见 `../artifacts/evidence.md`）。

```sh
make            # 构建（链接 pthread、m）
./simlab -t 4   # -t 指定线程数，1..8；不写入输出
make test       # 等价于 ./simlab -t 1
```
