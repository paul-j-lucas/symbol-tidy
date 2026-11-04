#include <vector>

struct S {
  S() { }
};

int main() {
  std::vector<S> v;
  (void)v;
}
