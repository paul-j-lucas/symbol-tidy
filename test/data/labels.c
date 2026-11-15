#include <stdio.h>

static void hello( void ) {
  printf( "hello, world\n" );
}

static void f( void ) {
  unused_label: hello();
}

int main() {
  goto used_label;
  used_label: f();
}
