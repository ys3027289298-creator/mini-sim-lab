# replay

输入回放校验：录制输入 → 按帧喂回仿真 → 逐帧比对基线轨迹。

`make test` 跑自检（全部写入临时目录，不动样例数据）。

## 用法

```sh
make                                   # 构建 ./replay
./replay gen 1 300 run.rec             # 生成一段录制（确定性）
./replay record run.rec run.traj       # 跑仿真，输出基线轨迹
./replay verify run.rec run.traj out.repro.rec
```

`verify` 退出码：`0` 完全一致；`1` 轨迹分歧（同时把输入截短到第一个分歧帧、
含该帧，写成最小复现）；`2` 录制文件非法（缺帧、时间戳倒退、格式错误）或 IO 错误。

## 批量

```sh
python3 tools/batch.py --replay-bin ./replay \
    --recordings recordings --baselines baselines --repro-dir /tmp/repro
```

逐段校验 `recordings/*.rec` 对 `baselines/*.traj`，汇总
`pass/diverge/error` 与 `failure_rate`，分歧段的最小复现写到 repro 目录。
跑前后对 baselines 做哈希校验，样例数据被改动会直接报错退出。

## 文件格式

录制（`.rec`）：`replay 1` / `seed N` / 每帧一行 `frame i t SEC input HEX`，
帧号必须连续递增，时间戳不得倒退。

轨迹（`.traj`）：`traj 1` / `seed N` / 每帧一行
`frame i tick n x .. y .. vx .. vy .. flags HEX rng HEX`，
位置类字段容差 1e-6，tick/flags/rng 精确比对。

`recordings/` 与 `baselines/` 是提交的样例数据；`make samples` 显式重新生成，
`verify`/`batch.py` 只读不写。
