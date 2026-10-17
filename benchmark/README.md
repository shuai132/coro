# Benchmark

Apple M2，macOS 27.0.1，Apple Clang 21.0.0，Release `-O3 -DNDEBUG`。
依赖固定为 async_simple `0c7a12b`、Asio 1.36.0，完整提交见 CMakeLists.txt。
coro 原生组使用 executor_loop，其余组使用 io_context；async_simple 使用测试适配器，不含线程池。

## 性能

单线程，预热后每轮 500000 次、11 轮中位数，单位 ns/op，越低越好。
立即完成为一次子协程调用；8 层嵌套共 9 个子协程；post 为一次挂起及恢复。
计时包含上下文创建、启动与销毁，保留各库默认分配器和缓存。

| 用例 | coro 原生 | coro + Asio | async_simple + Asio | Asio |
|---|---:|---:|---:|---:|
| 立即完成 | 27.15 | 26.98 | 29.85 | 16.32 |
| 8 层嵌套 | 246.20 | 245.90 | 261.89 | 258.84 |
| post 恢复 | 59.11 | 92.46 | 56.07 | 62.53 |

## 资源消耗

每组每个用例单独运行 200000 次；CPU 为用户态与内核态总和，RSS 为进程峰值。
CPU/RSS 使用原始程序，分配计数使用独立插桩程序，两者分别在新进程运行。

| 用例 | 路径 | CPU ms | RSS MiB | C++ 分配次数 | 累计请求 B | 存活峰值 B |
|---|---|---:|---:|---:|---:|---:|
| 立即完成 | coro 原生 | 5.272 | 1.625 | 200,008 | 20,804,584 | 4,688 |
| 立即完成 | coro + Asio | 5.228 | 1.688 | 200,010 | 20,800,999 | 1,103 |
| 立即完成 | async_simple + Asio | 5.845 | 1.688 | 200,007 | 20,800,782 | 886 |
| 立即完成 | Asio | 3.241 | 1.656 | 10 | 1,267 | 1,242 |
| 8 层嵌套 | coro 原生 | 48.263 | 1.641 | 1,800,008 | 187,204,584 | 5,520 |
| 8 层嵌套 | coro + Asio | 48.282 | 1.703 | 1,800,010 | 187,200,999 | 1,935 |
| 8 层嵌套 | async_simple + Asio | 50.648 | 1.688 | 1,800,007 | 187,200,782 | 1,718 |
| 8 层嵌套 | Asio | 49.728 | 1.688 | 1,400,011 | 147,001,372 | 2,058 |
| post 恢复 | coro 原生 | 11.531 | 1.656 | 200,010 | 6,408,696 | 8,664 |
| post 恢复 | coro + Asio | 18.078 | 1.672 | 10 | 999 | 999 |
| post 恢复 | async_simple + Asio | 10.914 | 1.703 | 200,007 | 9,600,782 | 830 |
| post 恢复 | Asio | 12.076 | 1.672 | 10 | 1,251 | 1,226 |

分配计数覆盖 C++ new/new[]，不含直接 malloc/aligned_alloc；累计请求量不等于峰值占用。
RSS 包含运行库，所有组完成后的已追踪存活字节均为 0。数据只代表单线程、单条任务链。

## 运行

需要 CMake 3.18+、C++20 和异常支持；首次启用时下载固定版本依赖。

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
  -DCORO_BUILD_TEST=OFF -DCORO_BUILD_EXAMPLES=OFF -DCORO_BUILD_BENCHMARKS=ON
cmake --build build_release -j
ctest --test-dir build_release --output-on-failure
./build_release/benchmark/coro_benchmark 500000 11

# Linux/macOS 资源报告，需要 Python 3
python3 benchmark/resources.py \
  ./build_release/benchmark/coro_benchmark \
  ./build_release/benchmark/coro_benchmark_allocations 200000
```

输出为 Markdown 表格；`--diagnostics` 增加具体执行器与 any_io_executor 对照。
