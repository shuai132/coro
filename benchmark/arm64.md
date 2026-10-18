# ARM64 Linux Cachegrind 与堆分配记录

使用 14 个常规组件场景记录执行和内存开销，结果写入 [instructions-arm64.json](instructions-arm64.json)。
运行入口为 [record_instructions.py](record_instructions.py)，独立于第三方库对比，不下载 Asio 或 async_simple。

## 运行

在 ARM64 Ubuntu 24.04 中安装工具，然后从仓库根目录执行：

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends cmake make g++-13 python3 valgrind
python3 benchmark/record_instructions.py
```

Apple Silicon 上需要在 ARM64 Linux 虚拟机或容器内执行。脚本使用独立的 `build_release/instructions` 构建目录。
固定 GCC 13、Release `-O3 -g`、ARMv8-A 和默认库配置；Valgrind 要求 3.22 或更新版本。
工具和运行库版本会写入 JSON，比较结果时应确认环境与场景版本一致。

## 场景与测量范围

覆盖任务、嵌套调用、同步/异步回调、有缓冲/无缓冲通道、mutex、semaphore、event、condition_variable、latch、wait_group、when_all 和 when_any。
每个场景由单线程驱动 `executor_poll`，使用默认线程安全组件，预热 100 次后测量 10,000 次。

测量包含任务与组件创建、执行、调度及完整清理，排除程序启动、预热和结果打印。
同步场景实际挂起后再唤醒；`when_any` 包含剩余分支的收尾。
校验和与独立的泄漏检查版本验证场景正确性，调试追踪不进入测量程序。

## Cachegrind 指标

`metrics.<场景>` 保留 Cachegrind 的全部 13 项原始计数，数值均为整个测量区间的次数：

| 字段 | 含义 |
|---|---|
| `Ir` | 执行指令数 |
| `I1mr` / `ILmr` | 一级 / 末级指令缓存读 miss |
| `Dr` / `Dw` | 数据内存读取 / 写入次数 |
| `D1mr` / `D1mw` | 一级数据缓存读 / 写 miss |
| `DLmr` / `DLmw` | 末级数据缓存读 / 写 miss |
| `Bc` / `Bcm` | 条件分支次数 / 预测失败次数 |
| `Bi` / `Bim` | 间接分支次数 / 预测失败次数 |

缓存模型固定为 I1/D1 各 32 KiB、8 路，LL 1 MiB、16 路，缓存行均为 64 字节，并写入 JSON。
miss 和分支预测失败是这个模型的模拟值，不是硬件实测值，也不能直接换算为耗时。
预热时插桩关闭，因此只预热程序和分配器，不预热模拟缓存或预测器。
计数可能随代码/数据地址布局变化，即使工具版本和工作量相同也不保证每次完全一致。

原始计数可用于计算比例，例如数据一级 miss 率为 `(D1mr + D1mw) / (Dr + Dw)`，条件分支预测失败率为 `Bcm / Bc`；分母为零时不定义比例。
`Dr` / `Dw` 包含栈、堆及全局数据访问，单位为次数，不是字节。

## 堆分配指标

同一份场景代码另编译为 `coro_instructions_allocations`，通过已有的 [allocation_counter.hpp](allocation_counter.hpp) 在独立原生进程中统计。
计数器只链接到这个版本，避免影响 Cachegrind 的指令和缓存计数。结果合并到同一 `metrics.<场景>`：

| 字段 | 含义 |
|---|---|
| `new_calls` | 区间内成功的 C++ `new/new[]` 分配次数 |
| `new_requested_bytes` | 这些分配累计请求的字节数 |
| `new_peak_live_bytes` | 这些分配同时存活的请求字节数峰值 |
| `new_live_bytes` | 区间结束时，这些分配仍存活的请求字节数 |

两个测量版本使用相同的输入、预热和区间边界；统计包含本轮资源清理，排除区间外分配。
支持普通、数组、对齐及 nothrow 分配；不包含直接 `malloc/aligned_alloc`、栈、分配器元数据/碎片或计数器自身的额外空间，也不等于 RSS。
预热或更早创建的分配不计入这四项；结束存活量可能是缓存保留，不能单凭非零判定泄漏。

## 结果与 CI

脚本校验两个版本的场景列表、工作量、校验和及计数格式，只有全部采集成功才替换结果文件。
默认更新仓库中的 JSON，也可以指定其他输出位置用于比较；原始测量文件保留在构建目录，可用 `cg_annotate` 定位到函数和源码行：

```bash
python3 benchmark/record_instructions.py --output build_release/instructions/new.json
cg_annotate build_release/instructions/channel_unbuffered.cgout
```

CI 运行相同采集流程，将 JSON 和原始 `.cgout`、`.allocations.json` 保存为 artifact，不自动提交、不根据指标变化阻断。
测量边界和验证细节见 [设计说明](../docs/plan/arm64-instruction-benchmark.md)。
