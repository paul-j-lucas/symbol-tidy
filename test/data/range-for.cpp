#include <stdio.h>

struct S {
  using const_pointer = char const*;
  using const_iterator = const_pointer;

  const_iterator begin() const {
    return MESSAGE;
  }

  const_iterator end() const {
    return MESSAGE + (sizeof(MESSAGE) / sizeof(MESSAGE[0]));
  }

  static constexpr char MESSAGE[] = "hello, world\n";
};

int main() {
  S const s;
  for ( auto c : s )
    putchar( c );
}
