#include <exception>

struct late_error : std::exception {
  char const* what() const noexcept override {
    return "late";
  }
};

void f() {
  try {
    throw late_error();
  }
  catch ( std::exception const& ) {
  }
}
