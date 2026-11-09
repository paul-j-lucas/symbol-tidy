#include <exception>

struct base_error : std::exception {
  char const* what() const noexcept override {
    return "base";
  }
};

struct derived_error : base_error {
  char const* what() const noexcept override {
    return "derived";
  }
};

int main() {
  try {
    throw derived_error();
  }
  catch ( std::exception const& ) {
  }
}
