#include <exception>

struct inline_error : std::exception {
  char const* what() const noexcept override {
    return "inline";
  }
};

struct out_of_line_error : std::exception {
  char const* what() const noexcept override;
};

char const* out_of_line_error::what() const noexcept {
  return "out_of_line";
}

int main() {
  try {
    throw inline_error();
  }
  catch ( std::exception const& ) {
  }
  try {
    throw out_of_line_error();
  }
  catch ( std::exception const& ) {
  }
}
