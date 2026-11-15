// Declaration in extern "C", definition outside it: expect "(move linkage
// specification to definition at line N)".
extern "C" {
  static void lk_moved( int );
}
static void lk_moved( int ) {
}

// Declaration and definition both in extern "C": expect no annotation.
extern "C" {
  static void lk_both( int );
  static void lk_both( int ) {
  }
}

// The same with attributes as well: expect "(move attributes and linkage
// specification to definition ...)".
extern "C" {
  static void lk_attr( int ) __attribute__((nothrow));
}
static void lk_attr( int ) {
}

[[noreturn]] static void cpp_attr( int );
[[noreturn]] static void cpp_attr( int ) {
  for (;;) { }
}

int main() {
  lk_moved( 1 );
  lk_both( 2 );
  lk_attr( 3 );
  cpp_attr( 4 );
}
