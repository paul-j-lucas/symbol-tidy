// Declaration in extern "C", definition outside it: expect "(move linkage
// specification to definition at line N)".
extern "C" void lk_moved( int );
void lk_moved( int ) {
}

// Declaration and definition both in extern "C": expect no annotation.
extern "C" void lk_both( int );
extern "C" void lk_both( int ) {
}

// The same with attributes as well: expect "(move attributes and linkage
// specification to definition ...)".
extern "C" void lk_attr( int ) __attribute__((nothrow));
void lk_attr( int ) {
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
