#include <stdio.h>

void hello( void ) {
  printf( "hello, world\n" );
}

void f( void ) {
  unused_label: hello();
}

int main() {
  goto used_label;
  used_label: f();
}
