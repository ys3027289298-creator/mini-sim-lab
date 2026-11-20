# fixedstep

`make test` 跑自检。

## 断点续跑

`Clock` 可以通过固定 56 字节的存档块保存/恢复：

- `clock_save(const Clock*, unsigned char* buf, size_t cap)` → 写入固定大小存档，成功返回 `FIXEDSTEP_SAVE_SIZE`（56），否则返回 `-1`。
- `clock_load(Clock*, const unsigned char* buf, size_t len)` → 成功返回 `0` 并填充时钟，坏存档返回 `-1` 且不改动目标时钟。

存档记录固定步计数 `total_steps`、残留累积量 `acc`（以及由它导出的 `alpha`）、`step`、`max_catchup` 和 `dropped`。所有 `double` 按原始 64 位模式存储，恢复是位精确的，跨存档点不会引入误差。

格式为显式小端布局（无结构体填充）：magic、version、配置、计数、累积量、保留字，最后是覆盖前 48 字节的 FNV-1a 校验和。长度不符、magic/版本/保留字不对、校验和不匹配、NaN/Inf、`step <= 0`、`acc` 不在 `[0, step)`、计数字段越界，都会拒绝加载。
