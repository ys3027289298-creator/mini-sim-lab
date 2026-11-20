# balance

确定性战斗数值回归：固定种子批量跑对局 → 记录伤害分布与胜率 → 与提交的基线
逐项比对，偏移超过阈值就报出「哪个种子、哪一项」，并用最少对局数给出最小复现。

数值全部集中在 `data/numbers.txt`，改数值就是改这张表；回归只读这张表，
跑前跑后做哈希校验，表被改动会直接报错退出。

## 用法

```sh
make                                   # 构建 ./balance
make test                              # 自检 + 对基线跑一遍回归
./balance run --seed 7 --table data/numbers.txt        # 跑单种子，打印指标与逐局摘要
./balance selftest --table data/numbers.txt            # 只跑内部自检
```

批量回归：

```sh
python3 tools/regress.py --balance-bin ./balance \
    --table data/numbers.txt --baselines baselines \
    --seeds 1:20 --repro-dir /tmp/repro
```

退出码：`0` 全部在阈值内；`1` 有种子超阈值（同时写出最小复现）；`2` 用法/IO/
基线缺失/种子范围非法/数值表被改动。

`--seeds A:B` 是闭区间；也接受单个 `A`。非整数、起点大于终点、超出 uint64、
超过 10 万个种子、超出基线覆盖范围，都会以 `2` 报错，不会静默跳过。

## 改了数值以后

`--write-baseline` 显式重新生成 `baselines/baseline.json`（这是唯一会写基线的路径）。
先用新表跑一次回归确认哪些种子、哪些项会动，再决定是接受新基线还是回退数值。

## 比对项与阈值

逐项比较，每项阈值为 `max(abs, rel * |基线值|)`：

| 项 | abs | rel |
|---|---|---|
| `win_rate` | 0.02 | — |
| `crit_rate` / `miss_rate` | 0.01 | 5% / 10% |
| `dmg_dealt_mean` / `dmg_taken_mean` / `avg_rounds` | — | 2% |
| `dmg_dealt_p50` / `p90` | — | 3% |
| `dmg_dealt_p99` / `dmg_dealt_max` | — | 5% |
| `hist_*_rate`（伤害直方图占比） | 0.01 | 10% |
| `hist_*_count`（伤害直方图计数） | 2 | 10% |

`wins`、`hits`、`dmg_dealt_sum` 这类整数计数只存进基线做参考，不参与比对。
阈值可用 `--thresholds FILE` 覆盖，文件是同样键名的 JSON，例如：

```json
{"win_rate": {"abs": 0.03, "rel": 0.0}, "hist_*": {"abs": 0.01, "rel": 0.1}}
```

## 最小复现

比赛是串行 RNG：第 N 局之前的所有局共用一条流，所以任意前缀都是稳定切片。
回归对每个超阈值的种子，二分/线性找出「从第 D 局起仍会超阈值」的最小 D，
写成 `seed-<S>.repro.json`，记录种子、对局数、`max_rounds`、数值表哈希和各项
的基线/实测/允许偏差。

```sh
python3 tools/regress.py ... --verify-repro /tmp/repro/seed-7.repro.json
```

`--verify-repro` 用同一张表重跑该复现，确认到 D 局仍超、而 D-1 局不超
（即确实最小）。表哈希不符会拒绝执行。

## 文件

- `src/balance.c` — 模拟器（splitmix64 + 定步长；`run`/`selftest` 两个模式）
- `data/numbers.txt` — 数值表（唯一数值来源）
- `baselines/baseline.json` — 提交的基线，含每种子逐局摘要与逐次命中伤害
- `tools/regress.py` — 批量回归、阈值比对、最小复现
