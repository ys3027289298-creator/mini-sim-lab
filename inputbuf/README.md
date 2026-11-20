# inputbuf

连招输入缓冲与容错判定，`make test` 跑自检。

- 每个动作带可配的 `pre`（提前按的缓冲帧）/ `post`（延后容忍帧），命中区间 `[F-pre, F+post]`
- 同一帧的多个按键合并为一个 mask 事件，重复按同一键幂等（`log_finalize` 排序后折叠）
- `match_strict` 是手工逐帧精确判定的参考实现；`pre=post=0` 或 `BufferCfg.enabled=0` 时行为与它完全一致
- `seq_validate` 拒绝负窗口、帧序非严格递增、相邻动作窗口重叠（边界相触也算重叠）
- `hit_curve` 把对称窗口从 0 扫到最大，输出每点的动作命中率与整套连招成功率；遇到重叠窗口自动截断
- `make csv` 额外把命中曲线写到 `../artifacts/inputbuf_hit_curve.csv`（可选，自检本身不依赖它）
