# Benchmark

Apple M2，macOS 27.0.1，Apple Clang 21.0.0，Release `-O3 -DNDEBUG`。
依赖固定为 async_simple `0c7a12b`、Asio 1.36.0，完整提交见 CMakeLists.txt。
主表统一使用单线程 Asio io_context。

## 性能

单线程，预热后每轮 500000 次、11 轮中位数，单位 ns/op，越低越好。
立即完成为一次子协程调用；8 层嵌套共 9 个子协程；post 为一次挂起及恢复。
计时包含上下文创建、启动与销毁，保留各库默认分配器和缓存。

默认配置（帧缓存关闭）：

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 27.15 | 28.44 | 16.17 |
| 8 层嵌套 | 249.79 | 265.06 | 259.35 |
| post 恢复 | 49.49 | 55.87 | 62.65 |

开启 `CORO_ENABLE_FRAME_CACHE`，使用默认 2 槽、单帧上限 512 B，其余设置相同：

| 用例 | coro | async_simple | Asio |
|---|---:|---:|---:|
| 立即完成 | 10.08 | 29.69 | 17.40 |
| 8 层嵌套 | 226.74 | 260.10 | 256.43 |
| post 恢复 | 49.18 | 55.12 | 61.72 |

## 资源消耗

每组每个用例单独运行 200000 次；CPU 为用户态与内核态总和，RSS 为进程峰值。
CPU/RSS 使用原始程序，分配计数使用独立插桩程序，两者分别在新进程运行。

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B | 保留 B |
|---|---|---:|---:|---:|---:|---:|---:|
| 立即完成 | coro | 5.322 | 1.703 | 200,009 | 20,800,974 | 1,078 | 0 |
| 立即完成 | async_simple | 5.699 | 1.703 | 200,007 | 20,800,782 | 886 | 0 |
| 立即完成 | Asio | 3.333 | 1.672 | 10 | 1,267 | 1,242 | 0 |
| 立即完成 | coro（缓存） | 2.010 | 1.672 | 10 | 1,078 | 1,078 | 296 |
| 8 层嵌套 | coro | 48.707 | 1.703 | 1,800,009 | 187,200,974 | 1,910 | 0 |
| 8 层嵌套 | async_simple | 51.597 | 1.703 | 1,800,007 | 187,200,782 | 1,718 | 0 |
| 8 层嵌套 | Asio | 50.225 | 1.688 | 1,400,011 | 147,001,372 | 2,058 | 0 |
| 8 层嵌套 | coro（缓存） | 46.421 | 1.688 | 1,400,011 | 145,601,182 | 1,910 | 208 |
| post 恢复 | coro | 9.704 | 1.688 | 9 | 974 | 974 | 0 |
| post 恢复 | async_simple | 10.850 | 1.703 | 200,007 | 9,600,782 | 830 | 0 |
| post 恢复 | Asio | 12.521 | 1.688 | 10 | 1,251 | 1,226 | 0 |
| post 恢复 | coro（缓存） | 9.817 | 1.672 | 9 | 974 | 974 | 344 |

分配计数覆盖 C++ new/new[]，不含直接 malloc/aligned_alloc；累计请求量不等于峰值占用。
RSS 包含运行库。缓存保留的帧内存在线程退出时释放，默认上限为每线程 1024 B；本轮实际为 208–352 B。
数据只代表单线程、单条任务链。

## 原生执行器补充对照

coro 使用 executor_loop，测量条件与主表相同。

| 用例 | 缓存关闭 ns/op | 缓存开启 ns/op |
|---|---:|---:|
| 立即完成 | 27.15 | 10.12 |
| 8 层嵌套 | 252.65 | 223.95 |
| post 恢复 | 61.53 | 59.07 |

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B | 保留 B |
|---|---|---:|---:|---:|---:|---:|---:|
| 立即完成 | coro 原生 | 5.398 | 1.641 | 200,008 | 20,804,584 | 4,688 | 0 |
| 立即完成 | coro 原生（缓存） | 2.006 | 1.625 | 9 | 4,688 | 4,688 | 296 |
| 8 层嵌套 | coro 原生 | 48.666 | 1.641 | 1,800,008 | 187,204,584 | 5,520 | 0 |
| 8 层嵌套 | coro 原生（缓存） | 44.365 | 1.656 | 1,400,010 | 145,604,792 | 5,520 | 208 |
| post 恢复 | coro 原生 | 11.937 | 1.656 | 200,010 | 6,408,696 | 8,664 | 0 |
| post 恢复 | coro 原生（缓存） | 11.852 | 1.672 | 200,010 | 6,408,696 | 8,664 | 352 |

## 运行

需要 CMake 3.18+、C++20 和异常支持；首次启用时下载固定版本依赖。

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON \
  -DCORO_ENABLE_FRAME_CACHE=OFF
cmake --build build_release -j
ctest --test-dir build_release --output-on-failure
./build_release/benchmark/coro_benchmark 500000 11

# Linux/macOS 资源报告，需要 Python 3
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 200000
```

将 `CORO_ENABLE_FRAME_CACHE` 改为 `ON`，重新构建并运行相同命令可复现缓存配置。
通过 `CORO_FRAME_CACHE_SLOTS` 和 `CORO_FRAME_CACHE_MAX_BYTES` 可静态调整容量（均为正整数）。
输出为 Markdown 表格；`--diagnostics` 增加 coro 原生执行器、具体执行器与 any_io_executor 补充对照。
