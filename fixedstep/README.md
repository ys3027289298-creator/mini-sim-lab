# fixedstep

确定性时间轴：固定步推进、渲染插值、输入按步对齐、可逐帧核对的回放。

`make test` 跑自检。

## 时间基准

- 时间单位是整数 tick（1 µs），不存在浮点累积。帧时间只在**累计时间**上向最近整数取整一次：
  `frame_tick[i] = round(Σframe_seconds[0..i] * 1e6)`，再差分得到 `delta_ticks`。
  因此 60 fps 跑 600 帧和 20 fps 跑 200 帧得到的总 tick、总步数、最终状态完全相同。
- 时钟状态全部为整数，账目恒等式可在任意帧核对：

  ```
  (total_steps + dropped) * step_ticks + acc == total_ticks
  ```

- `catchup_cap` 丢弃的步数计入 `dropped`，不会静默丢失；丢弃后 `acc` 只保留不足一步的余量。

## 状态与渲染

- 模拟只在固定步中推进，每步做一次 FNV-1a 哈希，回放可比对整条状态链。
- `render_sample()` 用 `alpha = acc/step` 在 `px,py → x,y` 之间插值，只读，不改变状态。
- 追帧上限不会影响渲染正确性，只影响 `dropped` 统计。

## 输入对齐

- 输入事件按其**绝对 tick** 对齐到步边界：`step_index = ceil(tick / step_ticks)`。
- 与帧切分无关：同一输入在 5 ms 帧和 33 ms 帧下落在同一步。
- 若该步被追帧上限丢弃，事件顺延到下一个实际执行的步，不会消失。

## 回放

- `record_run()` 记录整数 `delta_ticks` 序列和输入事件；`replay_run()` 只消费这些整数，不读墙钟。
- 同一录制回放任意次逐帧一致（`FrameState` 与哈希都相同）。
- `retimed_run()` 用不同的帧切分重放同一总 tick 与输入：步数、最终状态一致，逐帧快照允许不同。

## 核对点

`make test` 输出的关键项：`live_ledger`、`replay_frames_identical`、`replay_twice_identical`、
`retimed_step_count`、`pacing_steps_equal`、`live_vs_replay_final_zero`。
