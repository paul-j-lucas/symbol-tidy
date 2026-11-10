#define PJL_DISCARD_RV(FN_CALL) \
  do { [[maybe_unused]] typeof(FN_CALL) _rv = (FN_CALL); } while (0)

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
