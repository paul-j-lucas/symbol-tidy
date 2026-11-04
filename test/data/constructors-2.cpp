#include <stdio.h>

struct Int {
  Int() : _n{ 0 } { }
  Int( int n ) : _n{ n } { }

  int _n;
};

struct Int2 {
  Int i[2];
};

int main() {
  Int2 i2;
  printf( "%d\n", i2.i[0]._n );
}
