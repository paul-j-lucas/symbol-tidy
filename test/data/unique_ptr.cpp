#include <memory>

struct S {
  ~S() { }
};

int main() {
  std::unique_ptr<S> p{ new S };
}
