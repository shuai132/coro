// Register teardown before the translation unit that initializes async debug metadata.
void test_async_debug_static_lifetime();

namespace {
struct static_async_debug_probe {
  ~static_async_debug_probe() {
    test_async_debug_static_lifetime();
  }
};

static_async_debug_probe probe;
}  // namespace
