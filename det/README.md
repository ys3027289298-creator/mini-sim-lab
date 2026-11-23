# Determinism evidence for mini-sim-lab

固定的可复现证据：`fixedstep`、`pool`、`rng` 三处的确定性此前没有任何验证。
本目录引入一个**共享确定性上下文**（`detcontext`），并为每个模块提供证据驱动，
用同一套规则证明：两次运行逐位一致、线程数无关、关闭共享时输出不变、
非法参数按模块定位报错。

## 三个模块各自给出什么

| 模块 | 逐位哈希 | 线程无关性对比表 | 复现命令 |
|---|---|---|---|
| fixedstep | `fixedstep.state_hash` | `fixedstep.threads.{1,2,4,8}.digest` + `table_equal` | 见下 |
| pool | `pool.state_hash` | `pool.threads.{1,2,4,8}.digest` + `table_equal` | 见下 |
| rng | `rng.state_hash` | `rng.threads.{1,2,4,8}.digest` + `table_equal` | 见下 |

所有哈希都是 **FNV-1a 64**，输入是模块自己定义的**规范状态镜像**（小端、
定长字段），不是结构体内存：结构体填充字节不确定，直接哈希会让两次运行
结果不同。规范镜像见各 `*/src/evidence.c` 顶部注释。

## 一键复现（可复制粘贴）

```sh
cd /home/ys/feedback/uidreczz28cxBSiGJR7

# 0) 从零构建（保留原 make test 语义）
make clean && make

# 1) 完整确定性验证（自检不变/两次运行/单核/关闭共享/线程表/非法参数/快照往返/golden）
./det/verify.sh

# 2) 单条命令复现，并列出每个模块的逐位哈希与整份 stdout 的 sha256
./det/reproduce.sh
./det/reproduce.sh --tsv          # 便于 diff 的表格

# 3) 逐模块手动比对版本化 golden（整份 stdout 逐位）
for m in fixedstep pool rng; do
  ./"$m"/evidence --shared | cmp - det/evidence/golden-"$m".txt \
    && echo "$m: byte-identical to golden"
done

# 4) 关闭共享上下文：必须与 --shared 输出完全相同
for m in fixedstep pool rng; do
  cmp <(./"$m"/evidence --shared) <(./"$m"/evidence --no-shared) \
    && echo "$m: --no-shared identical"
done
```

当前冻结的基准哈希（`det/evidence/golden-summary.tsv`，stdout 为整份驱动的
`sha256`，两次运行/单核/关闭共享均须为 1）：

| 模块 | state_hash | table_hash | stdout sha256 |
|---|---|---|---|
| fixedstep | `e7fe153bea9b6f14` | `c750c00847c3bf12` | `e493bd39d5c5e969…` |
| pool | `6ebe0839e7f59bd9` | `1068b6865ddbfafd` | `524466d92d0bdc6c…` |
| rng | `b25cdaf7b6c26fef` | `832fac4bd8218efe` | `d7293b3d4558679e…` |

已在 `-O0/-O2/-O3/-Os` 以及 ASan+UBSan 下逐一复核，输出均与 golden 逐位一致。

## 三处各自的证据细节

### fixedstep —— 固定步进累积器

- 逐位哈希：72 帧周期调度 + 每 37 帧 backlog 尖峰，`state_hash` 覆盖
  step/acc/alpha/dropped/max_catchup/total_steps 的规范镜像。
- 线程无关性：64 个 tile，每个 tile 独立时钟，结果按 **索引顺序** 折叠；
  `threads.{1,2,4,8}.digest` 必须全相等且 `table_equal=1`。
- 语义保持：`--load` 后仅 `--advance N` 推进，故
  `dump@100 + advance50` 与 `warmup100 + frames50` 得到同一状态哈希。

### pool —— 定容对象池

- 逐位哈希：确定性操作脚本（分配/释放/探测陈旧句柄），`state_hash` 覆盖
  每槽 in_use/gen/value、used、high_water 及未释放句柄摘要。
- 线程无关性：每个 job 拥有独立池与独立 salt，无共享可变状态；按索引折叠。
- 陈旧句柄：`gen` 递增使旧句柄失效，摘要中含未释放句柄，防止“只对齐计数
  却放过悬垂句柄”。

### rng —— 随机流

- 逐位哈希：混合 `next`/`range`/`fork` 的确定抽取序列，`state_hash` 是
  stream 的 8 字节状态。
- 线程无关性：每个 job 独立流、独立写入槽位，按索引折叠。
- 快照/分叉：`draw_digest` 内部显式再做一次快照-恢复，确保摘要变化只可能
  来自流本身，而不是簿记。

## 共享确定性上下文约定

- 种子：模块流种子 = `det_unit_seed(seed, "<module>")`，`seed` 默认是固定
  常量（`det_cli_default_seed`），**从不**读取时钟、环境变量或 `rand()`。
- 状态：模块状态块由 `detunit` 分配并清零；`--no-shared` 时用私有清零块，
  两条路径的规范字节一致，因此输出必须逐位相同。
- 快照：`--dump`/`--load` 走固定小端格式，带 FNV-1a 校验和；`--load` 会
  校验文件名对应的单元存在且尺寸匹配，跨模块加载会被模块名拒绝。

## 非法参数报错定位

每个驱动在产生任何输出**之前**校验全部参数。错误写到 stderr，格式为
`<module>: <option>: <reason>`，退出码 2。例如：

```
fixedstep: --frames: 0 out of range [1, 1000000]
pool: threads: 8,1 is not strictly ascending
rng: --seed: expected an unsigned integer, got '-1'
fixedstep: load: '.../rng.only' contains units [rng], expected 'fixedstep'
```

## 文件

| 路径 | 作用 |
|---|---|
| `det/detcontext.{h,c}` | 上下文、确定性 arena、哈希、规范编解码、快照、并行-for |
| `det/cli.{h,c}` | 共享参数解析，错误统一按模块定位 |
| `det/evidence.{h,c}` | `--shared/--no-shared/--dump/--load` 的共享会话 |
| `det/verify.sh` | 全部确定性检查，退出码 0 表示全过 |
| `det/reproduce.sh` | 单条复现命令，打印各模块哈希与整份 stdout 的 sha256 |
| `det/evidence/golden-*.txt` | 版本化的期望输出（逐位比对基准） |
| `det/evidence/golden-summary.tsv` | 冻结的基准哈希表 |
| `det/evidence/baseline-*.txt` | 改造前的 `make test` 输出（自检不得变化） |
| `*/src/evidence.c` | 各模块的证据驱动 |
