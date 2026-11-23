# fixedstep

`make test` 跑自检（默认启用共享确定性上下文）。

上下文参数与验收说明见顶层 `README.md`；一键验收：`./tests/acceptance.sh`。
关闭共享、只跑原始自检：`./fixedstep --shared=off`（输出与改造前逐位一致）。
