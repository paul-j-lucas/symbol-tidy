#include <exception>

struct unused_error : std::exception {
  char const* what() const noexcept override {
    return "unused";
  }
};

int main() {
}
