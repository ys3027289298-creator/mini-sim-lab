# mini-sim-lab

游戏运行时基础设施，C 实现，每目录独立构建可自测。

| 目录 | 内容 |
|---|---|
| `detctx/` | 共享确定性上下文：splitmix64 流、参数解析、确定性并行 for、FNV 校验和（仅依赖 libc/pthread） |
| `fixedstep/` | 固定步长累积器、插值 alpha、追帧上限 |
| `pool/` | 定容对象池、代际句柄、悬垂检测 |
| `rng/` | 随机流、快照与分叉、无偏范围 |
| `tests/` | `acceptance.sh` 确定性验收脚本（无第三方依赖） |

## 构建与运行

```sh
make            # 构建三个模块（gcc/clang 均可，零 warning）
make test       # 各自跑自检
make clean
```

每个二进制接受统一的上下文参数：

```
--seed=N         64 位种子，0..18446744073709551615（默认 42）
--threads=N      worker 线程数，1..256（默认 1）
--shared=on|off  是否启用共享确定性上下文（默认 on）
-h, --help       用法
```

非法参数（越界线程数、负/溢出 seed、未知选项等）打印 `error:` 到 stderr 并以
退出码 2 失败；`--shared=off` 时只运行原始自检，输出与改造前逐位相同。

## 确定性验收（无第三方依赖）

```sh
./tests/acceptance.sh
```

脚本对 fixedstep / pool / rng **逐模块**验证：

1. **两次运行逐位一致** —— 同参数连跑两次，整文件 `cmp` 相等；外加 20 次重复
   16 线程运行全部一致。
2. **线程数无关性** —— 线程数 1/2/4/16/32/64/128（含超过物理核的超订）以及
   `taskset -c 0` 单核约束下，输出载荷 sha256 与程序自打印的 `checksum=` 恒定。
   实现保证：每个 item 的结果是 `(seed, item 索引)` 的纯函数，worker 只写各自
   私有槽位，按索引顺序归约，因此与调度无关。
3. **关闭共享即等价旧版** —— 用 `git show HEAD:<mod>/src/<mod>.c` 现场重建原始
   程序，`--shared=off` 的输出与其 `cmp` 逐位相等。
4. **非法参数报错** —— 10 类非法输入全部被拒并带 `error:` 消息，6 类边界合法
   值全部接受。
5. **依赖审计** —— `ldd` 仅出现 libc/libm/pthread/ld-linux，源码无第三方 include。

另可用 ASan/UBSan/TSan 做强化验证（编译器自带，非运行时第三方依赖）：

```sh
make CFLAGS="-O1 -g -std=c11 -pthread -fsanitize=address,undefined"
# TSan 在较新内核上需要 setarch -R 关闭 ASLR（TSan 的已知内核兼容问题）
make CFLAGS="-O1 -g -std=c11 -pthread -fsanitize=thread"
setarch "$(uname -m)" -R ./rng/rng --threads=16
```

最近一次完整验收结果见 `artifacts/acceptance-report.txt`。
