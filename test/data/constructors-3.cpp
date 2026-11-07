#include <vector>

struct S {
  S() { }

  explicit S( char const *arg ) {
    (void)arg;
  }

};

int main() {
  std::vector<S> v{ 10 };
}
