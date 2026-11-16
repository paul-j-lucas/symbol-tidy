#include "extern-macro.h"

DEFINE_HELPER( helper )
int x = OTHER;

int main( void ) {
  return helper() + x;
}
