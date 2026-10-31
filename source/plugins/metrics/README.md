# metrics（性能与费用统计插件）

采集每次模型调用的：

- 轮次 / 步数
- prompt / completion / total tokens
- 缓存命中率
- 输出速度（completion tokens / 模型耗时）
- 上下文占用（prompt tokens / 1M context window）
- 估算费用（USD）

## 文件

- `metrics_collector.*`：实现 `agent::core::Tracer`，累计指标；`OnTurnStart()` 由宿主每轮调用
- `cost_estimator.*`：DeepSeek 价格表、peak/off-peak 判断、费用计算
- `metrics_formatter.*`：输出一行状态文本

## 使用

```powershell
# 单轮 + 统计
.\build\vs2022\bin\Debug\agent-cli.exe --stats "hello"

# 交互模式：采集始终开启
# --stats 每轮自动打印；/stats 随时查看当前会话
.\build\vs2022\bin\Debug\agent-cli.exe --stats --interactive
```

统计输出到 `stderr`，不污染模型回答的 stdout。

## 费用估算说明

- 价格按 `(provider, model)` 查找，不只看 model；
- 默认使用中国区官方价格（CNY / 1M tokens，off-peak），provider 为 `deepseek`；
- 中国区 peak：北京时间 09:00-12:00、14:00-18:00，周一至周五；其他时间为 off-peak；
- 国际区价格用 USD，peak 按 UTC 01:00-04:00、06:00-10:00 判断；
- 中国法定节假日暂未处理；
- 输出中的 `≈` 表示估算，不是账单；
- 未知 provider/model 或缺少 usage 时显示 `n/a`；
- 价格可用 `CostEstimator::SetPricing(provider, model, pricing)` 覆盖，支持不同币种；
- 其他供应商（OpenAI/Azure/OpenRouter/阿里云等）需要各自的 pricing profile。
