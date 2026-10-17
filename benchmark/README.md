# Benchmark

Apple M2，macOS 27.0.1，Apple Clang 21.0.0，Release `-O3 -DNDEBUG`。
依赖固定为 async_simple `0c7a12b`、Asio 1.36.0，完整提交见 CMakeLists.txt。
主表统一使用单线程 Asio io_context。

## 性能

单线程，预热后每轮 500000 次、11 轮中位数，单位 ns/op，越低越好。
立即完成为一次子协程调用；8 层嵌套共 9 个子协程；post 为一次挂起、排队及恢复。
post 使用 coro 的 `callback_awaiter`、async_simple 的 `Yield` 和 Asio 的 `post(deferred)`。
Asio 直接等待操作，避免 `use_awaitable` 额外创建协程帧；对比的是公开 API 的端到端开销。
计时包含上下文创建、启动与销毁；除下述帧缓存配置外，其余使用各库默认设置。

coro 和 Asio 均开启帧缓存：coro 使用默认 2 槽、单帧上限 512 B，Asio 使用默认缓存。
async_simple 在两组中均保持默认无帧缓存，作为参照。

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 10.16 | 28.35 | 15.84 |
| 8 层嵌套 | 221.94 | 258.50 | 249.20 |
| post 恢复 | 48.79 | 54.78 | 47.94 |

coro 和 Asio 均关闭帧缓存：`CORO_ENABLE_FRAME_CACHE=OFF`，定义 `ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`；async_simple 保持默认分配器。
该组排除了帧缓存策略差异，仍包含帧分配与执行器调度开销，Asio 的其他调度缓存保持默认。

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 26.53 | 28.31 | 28.02 |
| 8 层嵌套 | 244.24 | 258.64 | 259.51 |
| post 恢复 | 49.47 | 57.76 | 47.98 |

## 资源消耗

每组每个用例单独运行 200000 次；CPU 为用户态与内核态总和，RSS 为进程峰值。
CPU/RSS 使用原始程序，分配计数使用独立插桩程序，两者分别在新进程运行。

coro 和 Asio 均开启帧缓存：

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B |
|---|---|---:|---:|---:|---:|---:|
| 立即完成 | coro | 1.979 | 1.688 | 10 | 1,078 | 1,078 |
| 立即完成 | async_simple | 5.501 | 1.688 | 200,007 | 20,800,782 | 886 |
| 立即完成 | Asio | 3.149 | 1.672 | 10 | 1,275 | 1,250 |
| 8 层嵌套 | coro | 44.228 | 1.703 | 1,400,011 | 145,601,182 | 1,910 |
| 8 层嵌套 | async_simple | 52.082 | 1.688 | 1,800,007 | 187,200,782 | 1,718 |
| 8 层嵌套 | Asio | 49.451 | 1.688 | 1,400,011 | 147,001,380 | 2,066 |
| post 恢复 | coro | 9.586 | 1.688 | 9 | 974 | 974 |
| post 恢复 | async_simple | 10.786 | 1.688 | 200,007 | 9,600,782 | 830 |
| post 恢复 | Asio | 9.450 | 1.672 | 9 | 1,170 | 1,145 |

coro 和 Asio 均关闭帧缓存：

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B |
|---|---|---:|---:|---:|---:|---:|
| 立即完成 | coro | 5.475 | 1.703 | 200,009 | 20,800,974 | 1,078 |
| 立即完成 | async_simple | 5.816 | 1.672 | 200,007 | 20,800,782 | 886 |
| 立即完成 | Asio | 6.149 | 1.703 | 200,009 | 20,801,168 | 1,223 |
| 8 层嵌套 | coro | 50.056 | 1.703 | 1,800,009 | 187,200,974 | 1,910 |
| 8 层嵌套 | async_simple | 67.518 | 1.672 | 1,800,007 | 187,200,782 | 1,718 |
| 8 层嵌套 | Asio | 52.496 | 1.703 | 1,800,009 | 187,201,168 | 2,055 |
| post 恢复 | coro | 9.755 | 1.672 | 9 | 974 | 974 |
| post 恢复 | async_simple | 11.320 | 1.688 | 200,007 | 9,600,782 | 830 |
| post 恢复 | Asio | 9.507 | 1.672 | 9 | 1,168 | 1,119 |

分配计数覆盖 C++ new/new[]，不含直接 malloc/aligned_alloc；累计请求量不等于峰值占用。
RSS 包含运行库。缓存保留的帧内存在线程退出时释放，默认上限为每线程 1024 B。原生执行器另保留最多两个空回调对象，本机共 64 B，与帧缓存开关无关。
数据只代表单线程、单条任务链。

## 原生执行器

不使用协程；单线程串行 post，每个回调提交下一个回调，两端均接收相同的 `std::function<void()>`。
计时包含上下文创建、启动与销毁：500000 次、11 轮中位数；资源统计为独立进程运行 200000 次。

| 执行器 | post ns/op | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B |
|---|---:|---:|---:|---:|---:|---:|
| executor_loop | 28.20 | 5.596 | 1.625 | 6 | 8,280 | 8,280 |
| Asio io_context | 33.82 | 6.727 | 1.641 | 5 | 566 | 566 |

## 运行

需要 CMake 3.18+、C++20 和异常支持；首次启用时下载固定版本依赖。
以下命令开启 coro 和 Asio 的帧缓存：

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON \
  -DCORO_ENABLE_FRAME_CACHE=ON
cmake --build build_release -j
ctest --test-dir build_release --output-on-failure
./build_release/benchmark/coro_benchmark 500000 11

# Linux/macOS 资源报告，需要 Python 3
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 200000
```

通过 `CORO_FRAME_CACHE_SLOTS` 和 `CORO_FRAME_CACHE_MAX_BYTES` 可静态调整容量（均为正整数）。
输出为 Markdown 表格；`--diagnostics` 增加具体执行器与 any_io_executor 对照。
执行器单独测试（与帧缓存开关无关）：

```bash
./build_release/benchmark/coro_benchmark --executors 500000 11
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 200000 --executors
```

统一关闭帧缓存，使用独立构建目录：

```bash
cmake -S . -B build_release/benchmark_no_frame_cache -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON \
  -DCORO_ENABLE_FRAME_CACHE=OFF -DCMAKE_CXX_FLAGS=-DASIO_DISABLE_AWAITABLE_FRAME_RECYCLING
cmake --build build_release/benchmark_no_frame_cache -j
./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark 500000 11
python3 benchmark/resources.py \
  ./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark \
  ./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark_allocations 200000
```
