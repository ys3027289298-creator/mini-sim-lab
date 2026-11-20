# knockback

击退与硬直，固定步长（`SIM_DT = 1/60`）模拟。`make test` 跑自检。

## 规则

- **击退速度**：攻击方向单位化后乘 `HitSpec.speed`（上限 `SPEED_CAP = 40`）。
- **衰减曲线**（可配 `HitSpec.curve`）：
  - `CURVE_LINEAR`：第 k 帧（共 T 帧）速度为 `v0 * (T-k)/T`，硬直结束帧速度精确归零。
  - `CURVE_EXPO`：每帧乘 `decay ∈ (0,1]`，硬直结束帧强制归零。
- **硬直**：`stun_frames` 内 `fighter_can_act() == 0`，`try_move` 被拒绝。
- **撞墙**：位置钳制到场地边界，法向速度分量清零，切向分量保留 —— 沿墙滑动而不是穿过。
- **叠加规则**：硬直中再次受击，速度立即被新击退**替换**，硬直剩余帧取 `max(剩余, 新硬直)`。
- **校验**：`stun_frames < 0`、`speed < 0` 或 `speed > SPEED_CAP`、零向量方向、`decay` 越界，一律返回 -1 且角色状态不变。
