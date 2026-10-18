# ARM64 Linux Cachegrind 与堆分配记录设计

## 目标与范围

使用一个常规组件测试程序，在 ARM64 Linux 上通过 Cachegrind 记录指定代码区间的执行指令、内存访问、模拟缓存 miss 和分支预测计数。
同一份场景代码独立编译运行，复用现有 C++ 分配计数器，统计相同区间的分配与存活字节。
结果保存为仓库内固定 JSON 文件。
CI 使用相同流程生成记录和原始测量文件，作为 artifact 保存，不自动提交结果。

记录 Cachegrind 原生提供的全部 13 项计数，不设置退化阈值，不增加看板、多平台性能矩阵或耗时测量。
指令数反映所选环境和场景下的执行开销，不等于真实耗时。
内存访问次数不等于内存占用；堆统计通过已有 `benchmark/allocation_counter.hpp` 提供，不引入外部堆分析工具。
现有功能测试、Sanitizer 检查和第三方库性能对比继续保持独立。

## 固定配置

- 平台：ARM64 Linux；CI 使用 `ubuntu-24.04-arm`。
- 工具：GCC 13、Valgrind 3.22 或更新版本；精确版本写入结果，升级时重新生成记录。
- 构建：C++20、Release 优化并保留调试信息，用于指令归因；固定 ARMv8-A 目标，不使用 `-march=native`。
- 库配置：使用默认配置，开启异常，关闭帧缓存、协程本地存储和调试元数据。
- 执行方式：一个线程驱动 `executor_poll`，同步原语使用默认线程安全类型。
  单线程运行使场景顺序可控，并不将库切换为 `CORO_SINGLE_THREAD`。
- 测量程序关闭泄漏追踪和 Sanitizer；另用同一份场景代码进行正确性与泄漏检查。
- 启用缓存与分支预测模拟，I1/D1 固定各 32 KiB、8 路，LL 固定 1 MiB、16 路，缓存行均为 64 字节。
  使用固定模型而非主机自动探测值，配置写入 JSON；该模型不代表具体 ARM 硬件。
- 分配计数器只链接到 `coro_instructions_allocations`，独立进程原生运行，避免计数器开销进入 Cachegrind 数据。

只维护这一套测量配置，不测线程竞争和真实时间驱动的等待。
本地与 CI 的结果只有在工具链、运行库、配置和工作量一致时才适合直接比较。
记录编译器、Valgrind、glibc 和 libstdc++ 版本，避免把环境升级误认为代码变化。
缓存和分支模拟受地址布局影响，同一环境也可能有波动；只记录，不要求逐字一致。
Apple Silicon 本地更新记录时使用 ARM64 Linux 环境，macOS 原生运行结果不写入该记录。

## 场景

一个可执行程序通过参数选择场景，每个场景在独立进程中运行，输出独立的一组计数。

| 场景 | 工作量定义 |
|---|---|
| `task` | 一次立即完成的子协程调用及返回值处理 |
| `nested` | 一次固定深度的嵌套协程调用及完整返回 |
| `callback_sync` | 一次同步立即回调 |
| `callback_post` | 一次回调 post、挂起及恢复 |
| `channel_buffered` | 一条消息的有缓冲发送及接收 |
| `channel_unbuffered` | 一条消息的无缓冲交接，实际发生挂起及唤醒 |
| `mutex` | 一轮锁竞争、等待及 RAII 释放 |
| `semaphore` | 一轮无可用许可时的等待及释放唤醒 |
| `event` | 一轮未设置事件的等待及设置唤醒 |
| `condition_variable` | 一轮等待、通知及重新获取锁 |
| `latch` | 一轮等待及计数归零 |
| `wait_group` | 一轮任务登记、等待及完成 |
| `when_all` | 一组固定数量的子任务全部完成 |
| `when_any` | 一组固定数量的子任务竞争，并等待所有剩余任务收尾 |

默认每个场景运行 10,000 次，预热运行 100 次；工作量是可比较记录的一部分。
场景使用固定输入、固定任务数量和确定的执行顺序，不使用真实 sleep、I/O 或随机负载。
收发值、完成数量和校验和必须可验证，不能只创建组件而不使用。
回调正确性检查补充 executor-aware、void、move-only 和非默认构造结果覆盖，不要求每种组合都成为独立指标。

## 测量区间

使用 `--instr-at-start=no` 禁止进程启动时计数，只在测试代码中明确开始和停止。

```text
解析参数、选择场景
预热并验证预热结果，完整清理预热任务
CACHEGRIND_START_INSTRUMENTATION
  执行固定次数的场景
  完成所有任务并销毁本轮拥有的资源
CACHEGRIND_STOP_INSTRUMENTATION
验证测量结果、打印完成信息
```

区间包含该场景所需的任务创建、组件初始化、调度、循环、结果累计和清理。
程序启动、参数解析、预热、最终断言和打印在区间外。
预热期间不插桩，不预热模拟缓存及分支预测器；测量开始时模拟状态仍为初始状态。
每个进程只开启一次测量区间；用一个场景一个进程的方式避免计数混合。
不得在 `when_any` 返回时就停止测量，仍在运行的分支及辅助协程必须完成清理。

运行命令的形式如下，具体程序名和参数由实现保持一致：

```bash
valgrind --tool=cachegrind --instr-at-start=no \
  --cache-sim=yes --branch-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=1048576,16,64 \
  --cachegrind-out-file=build_release/instructions/task.cgout \
  ./build_release/instructions/coro_instructions task 10000
```

按原始输出的 `events:` 名称解析 `summary:` 中全部计数，不依赖终端日志的语言或列位置。
必需字段为 `Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw Bc Bcm Bi Bim`，详见 [指标说明](../../benchmark/arm64.md#cachegrind-指标)。
零次 miss 或分支是合法值；缺少任意必需字段、重复字段、负值或零指令数均视为采集失败。
保留工具未来可能添加的其他字段，不静默丢弃。
原始文件保留在构建目录，必要时用 `cg_annotate` 定位到文件、函数及源码行。

## 堆分配口径

分配版本在相同 `run()` 前后调用 `allocation_counter::begin()` / `end()`。
预热仍运行 100 次，正式测量 10,000 次；创建、调度及完整清理都在区间内，输出在区间外。
单个场景的分配结果输出为 JSON，包含场景名、次数、校验和及以下字段：

- `new_calls`：本区间成功的 C++ `new/new[]` 分配次数，包括对齐和 nothrow 形式。
- `new_requested_bytes`：这些分配累计请求的字节数。
- `new_peak_live_bytes`：这些分配同时存活的请求字节数峰值。
- `new_live_bytes`：区间结束时，这些分配仍存活的请求字节数。

只计区间内新分配的内存；释放预热或旧区间分配不影响本区间存活量。
不包含直接 `malloc/aligned_alloc`、栈、分配器元数据/碎片、计数器额外空间，不等于进程 RSS。
结束时非零可能是缓存保留，不能据此直接判定泄漏。
解析时核对场景、次数、校验和、四项非负整数及 `live <= peak <= requested`，允许合法的零次分配。
将原始分配 JSON 保留为 `<场景>.allocations.json`，由 CI 连同 `.cgout` 上传。

## 结果与使用流程

结果写入 `benchmark/instructions-arm64.json`，包含：

- 格式版本和场景版本；场景语义变化时更新场景版本。
- 平台、编译器、运行库、Valgrind 版本及实际构建配置。
- 每个场景的迭代次数、预热次数、缓存模拟参数、分支模拟开关及独立进程分配统计方式。
- `metrics.<场景>` 下的 Cachegrind 原始计数和四项分配计数；格式版本 3，工作量未变，仍为版本 1。

固定字段顺序和场景顺序，不写时间戳、PID、机器名、绝对路径或原始调用栈。
只在全部场景成功后替换结果文件；失败、缺失场景或缺失任意必需计数时返回非零，保留原记录。
不把编造的示例数值作为首份记录，首份 JSON 必须来自实际 ARM64 Linux 测量。

运行记录脚本可更新本地 JSON 文件。
CI 将结果写到构建目录并上传 artifact，不要求有噪声可能性的计数与仓库记录逐字相同。
CI 只因构建失败、场景校验失败或采集失败而失败；指标变化本身不阻断。

## 最小实现与验证

实现集中在 `benchmark/`：一份场景程序、一份 Python 记录脚本和一份 JSON 记录。
脚本自动构建并运行两个测量版本，核对场景列表，再合并结果；使用者仍只需执行原命令。
增加独立的可选 CMake 目标及一个 ARM64 CI job，不依赖 Asio、async_simple 或新测试框架。
Valgrind 只用于可选测量目标，不成为核心库及消费者的依赖。
使用同一份场景代码构建泄漏检查版本，启用 `CORO_DEBUG_PROMISE_LEAK`，结束时调用 `check_coro_leak()`。

按项目的测试优先约定，先验证场景结果、挂起路径与完整清理，再接入采集：

1. 用小工作量验证每个场景的结果及无泄漏，包含零次和单次的边界检查。
2. 验证记录脚本按名称读取全部计数，接受零次 miss，拒绝缺失、重复、损坏或不完整的测量结果。
3. 用区间外的额外纯计算、打印和分配验证测量边界；增加区间内工作量应增加指令、读写、分支及分配计数。
   分配计数器自检覆盖普通/数组/对齐/nothrow 分配、峰值与结束存活量、零字节分配、旧区间释放；所有场景小工作量结束时应完整释放本轮分配。
4. 在同一 ARM64 Linux 环境连续测量两次，核对差异；如有波动，先定位来源，再生成首份记录。
5. 完整检查编译日志与测试结果，更新中英文 README 中的使用入口。

## 参考

- [Cachegrind 手册](https://valgrind.org/docs/manual/cg-manual.html)：区间控制、全部计数、模拟参数与限制、原始文件格式及归因。
- [Valgrind 3.22 发布说明](https://valgrind.org/docs/manual/dist.news.html)：该版本增加 Cachegrind 区间控制接口。
- [Valgrind 支持平台](https://valgrind.org/info/platforms.html)：ARM64 Linux 支持范围。
- [GitHub ARM64 runner](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)：CI 运行环境。
