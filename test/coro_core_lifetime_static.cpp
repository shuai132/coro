// Keep this translation unit independent of coro headers so its initializer can run first.
void test_static_coroutine_lifetime();

namespace {
struct static_lifetime_probe {
  static_lifetime_probe() {
    test_static_coroutine_lifetime();
  }

  ~static_lifetime_probe() {
    test_static_coroutine_lifetime();
  }
};

static_lifetime_probe probe;
}  // namespace
