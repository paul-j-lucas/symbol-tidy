#include <stdio.h>

struct class_with_begin_end {
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

struct class_without_begin_end {
  using const_pointer = char const*;
  using const_iterator = const_pointer;

  static constexpr char MESSAGE[] = "hello, world\n";
};

inline class_without_begin_end::const_iterator
begin( class_without_begin_end const &obj ) {
  return obj.MESSAGE;
}

inline class_without_begin_end::const_iterator
end( class_without_begin_end const &obj ) {
  return obj.MESSAGE + (sizeof(obj.MESSAGE) / sizeof(obj.MESSAGE[0]));
}

int main() {
  class_with_begin_end const obj_with_begin_end;
  for ( auto c : obj_with_begin_end )
    putchar( c );

  class_without_begin_end const obj_without_begin_end;
  for ( auto c : obj_without_begin_end )
    putchar( c );
}
