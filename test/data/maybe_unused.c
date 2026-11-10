#include "pjl_config.h"

struct [[maybe_unused]] unused_struct {
  int x;
};

[[maybe_unused]] void unused_func() {
}

int f( char const *config ) {
  (void)config;
  return 0;
}

void g( char const *config ) {
  PJL_DISCARD_RV( f( config ) );
}

int main() {
  g( "x" );
}
