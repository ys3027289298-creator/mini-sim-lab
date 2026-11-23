# sim

固定步进实体仿真的回放复现框架，链接 fixedstep / pool / rng 三个库。

- `make` 产出两个二进制：
  - `sim`：修复后的确定性构建（纳秒定点 clock、按稳定 spawn id 分叉的实体私有 RNG 流、静态槽位分区、驱动线程统一回收）。
  - `sim-legacy`：改动前行为（double/fmod clock、工作线程共享 RNG、原子抢槽），仅用于复现漂移。
- 用法：`./sim <threads> <merge:0|1> <out.rec> [seed]`
  - `merge=1` 把同一组帧时间戳两两相加后喂入，即相同仿真时间用一半帧数提交，用于暴露累加顺序差异。
  - 输出文件按 `RecHeader | (StepRec, EntityRec...)...` 打包，逐字节可哈希对比；stdout 打印轨迹摘要（步数、spawn/despawn、FNV-1a）。
