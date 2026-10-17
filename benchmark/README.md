# Benchmark

Apple M2，macOS 27.0.1，Apple Clang 21.0.0，Release `-O3 -DNDEBUG`。
依赖固定为 async_simple `0c7a12b`、Asio 1.36.0，完整提交见 CMakeLists.txt。
主表统一使用单线程 Asio io_context。

## 性能

单线程，预热后每轮 1000000 次、11 轮中位数，单位 ns/op，越低越好。
立即完成为一次子协程调用；8 层嵌套共 9 个子协程；post 为一次挂起、排队及恢复。
计时包含初始化和清理；除帧缓存开关外，其余配置保持默认。

coro 和 Asio 均开启帧缓存：coro 使用默认 2 槽、单帧上限 512 字节，Asio 使用默认缓存。
async_simple 在两组中均保持默认无帧缓存，作为参照。

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 10.21 | 28.97 | 16.80 |
| 8 层嵌套 | 222.42 | 262.86 | 256.52 |
| post 恢复 | 47.41 | 57.98 | 50.01 |

coro 和 Asio 均关闭帧缓存：`CORO_ENABLE_FRAME_CACHE=OFF`，定义 `ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`；async_simple 保持默认分配器。
该组排除了帧缓存策略差异，仍包含帧分配与执行器调度开销，Asio 的其他调度缓存保持默认。

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 25.01 | 28.30 | 30.17 |
| 8 层嵌套 | 241.11 | 258.66 | 263.01 |
| post 恢复 | 48.88 | 55.14 | 47.33 |

## 资源消耗

每组每个用例单独运行 1000000 次；CPU 为用户态与内核态总和，RSS 为进程峰值。
CPU/RSS 使用原始程序，分配计数使用独立插桩程序，两者分别在新进程运行。

coro 和 Asio 均开启帧缓存：

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求（字节） | 存活峰值（字节） |
|---|---|---:|---:|---:|---:|---:|
| 立即完成 | coro | 9.957 | 1.672 | 10 | 1,078 | 1,078 |
| 立即完成 | async_simple | 28.375 | 1.688 | 1,000,007 | 104,000,782 | 886 |
| 立即完成 | Asio | 15.859 | 1.656 | 10 | 1,275 | 1,250 |
| 8 层嵌套 | coro | 221.716 | 1.703 | 7,000,011 | 728,001,182 | 1,910 |
| 8 层嵌套 | async_simple | 258.248 | 1.688 | 9,000,007 | 936,000,782 | 1,718 |
| 8 层嵌套 | Asio | 249.160 | 1.672 | 7,000,011 | 735,001,380 | 2,066 |
| post 恢复 | coro | 47.287 | 1.672 | 9 | 974 | 974 |
| post 恢复 | async_simple | 57.631 | 1.688 | 1,000,007 | 48,000,782 | 830 |
| post 恢复 | Asio | 47.044 | 1.656 | 9 | 1,170 | 1,145 |

coro 和 Asio 均关闭帧缓存：

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求（字节） | 存活峰值（字节） |
|---|---|---:|---:|---:|---:|---:|
| 立即完成 | coro | 26.511 | 1.703 | 1,000,009 | 104,000,974 | 1,078 |
| 立即完成 | async_simple | 30.552 | 1.672 | 1,000,007 | 104,000,782 | 886 |
| 立即完成 | Asio | 30.286 | 1.688 | 1,000,009 | 104,001,168 | 1,223 |
| 8 层嵌套 | coro | 263.932 | 1.688 | 9,000,009 | 936,000,974 | 1,910 |
| 8 层嵌套 | async_simple | 276.341 | 1.672 | 9,000,007 | 936,000,782 | 1,718 |
| 8 层嵌套 | Asio | 286.378 | 1.688 | 9,000,009 | 936,001,168 | 2,055 |
| post 恢复 | coro | 64.811 | 1.672 | 9 | 974 | 974 |
| post 恢复 | async_simple | 58.805 | 1.688 | 1,000,007 | 48,000,782 | 830 |
| post 恢复 | Asio | 49.486 | 1.656 | 9 | 1,168 | 1,119 |

分配计数覆盖 C++ new/new[]，不含直接 malloc/aligned_alloc；累计请求量不等于峰值占用。
RSS 包含运行库。缓存保留的帧内存在线程退出时释放，默认上限为每线程 1024 字节。原生执行器另保留最多两个空回调对象，本机共 64 字节，与帧缓存开关无关。
数据只代表单线程、单条任务链。

## 原生执行器

不使用协程；单线程串行 post，每个回调提交下一个回调，两端均接收相同的 `std::function<void()>`。
计时包含上下文创建、启动与销毁：1000000 次、11 轮中位数；资源统计为独立进程运行 1000000 次。

| 执行器 | post ns/op | CPU ms | RSS MiB | C++ 分配次数 | 累计请求（字节） | 存活峰值（字节） |
|---|---:|---:|---:|---:|---:|---:|
| executor_loop | 35.18 | 34.897 | 1.625 | 6 | 8,280 | 8,280 |
| Asio io_context | 34.50 | 33.433 | 1.625 | 5 | 566 | 566 |

## 运行

需要 CMake 3.18+、C++20 和异常支持；首次启用时下载固定版本依赖。
以下命令开启 coro 和 Asio 的帧缓存：

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON \
  -DCORO_ENABLE_FRAME_CACHE=ON
cmake --build build_release -j
ctest --test-dir build_release --output-on-failure
./build_release/benchmark/coro_benchmark 1000000 11

# Linux/macOS 资源报告，需要 Python 3
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 1000000
```

通过 `CORO_FRAME_CACHE_SLOTS` 和 `CORO_FRAME_CACHE_MAX_BYTES` 可静态调整容量（均为正整数）。
输出为 Markdown 表格；`--diagnostics` 增加具体执行器与 any_io_executor 对照。
执行器单独测试（与帧缓存开关无关）：

```bash
./build_release/benchmark/coro_benchmark --executors 1000000 11
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 1000000 --executors
```

统一关闭帧缓存，使用独立构建目录：

```bash
cmake -S . -B build_release/benchmark_no_frame_cache -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON \
  -DCORO_ENABLE_FRAME_CACHE=OFF -DCMAKE_CXX_FLAGS=-DASIO_DISABLE_AWAITABLE_FRAME_RECYCLING
cmake --build build_release/benchmark_no_frame_cache -j
./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark 1000000 11
python3 benchmark/resources.py \
  ./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark \
  ./build_release/benchmark_no_frame_cache/benchmark/coro_benchmark_allocations 1000000
```
