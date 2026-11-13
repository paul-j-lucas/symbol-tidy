// Plain case: no annotation.
static void unnecessary_plain( int );
static void unnecessary_plain( int n ) {
  (void)n;
}

// Has an attribute: expect "(move attributes to definition at line N)".
static void unnecessary_attr( void ) __attribute__((noreturn));
static void unnecessary_attr( void ) {}

// Recursion inside the body doesn't make the declaration necessary.
static int unnecessary_recursive( int );
static int unnecessary_recursive( int n ) {
  return n <= 1 ? 1 : n * unnecessary_recursive( n - 1 );
}

// External linkage: no annotation.
void unnecessary_extern( int );
void unnecessary_extern( int n ) {
  (void)n;
}

// Redeclaration of a library builtin: expect NO annotation.  If
// clang_Cursor_hasAttrs() counts implicit attributes (e.g., nothrow, const)
// you'll get a spurious "(move attributes to definition ...)" here.
int abs( int );
int abs( int x ) {
  return x < 0 ? -x : x;
}

// Redundant redeclaration after the definition: expect it reported, with
// nothing to move.
static void unnecessary_redundant( int n ) {
  (void)n;
}
static void unnecessary_redundant( int );

// Referenced before the definition.
static void necessary_before( void );
static void necessary_caller( void ) {
  necessary_before();
}
static void necessary_before( void ) {
}

// Mutual recursion; the declaration of necessary_mutual_b is needed by
// necessary_mutual_a.
static void necessary_mutual_b( void );
static void necessary_mutual_a( void ) { necessary_mutual_b(); }
static void necessary_mutual_b( void ) { necessary_mutual_a(); }

// Address taken before the definition.
static void necessary_ptr( void );
static void (*necessary_table[])( void ) = { necessary_ptr };
static void necessary_ptr( void ) {}

// Never referenced; left to the unused-symbols report (internal linkage).
static void necessary_dead( void );
static void necessary_dead( void ) {
}

// The parameter's type carries noreturn; the function itself has none.  Expect
// "unnecessary_fp_inline" reported with NO annotation.  A "(move attributes
// ...)" annotation means the type-spelling fallback matched the parameter's
// type: the false positive.
static void unnecessary_fp_inline( void (*)(void) __attribute__((noreturn)) );
static void unnecessary_fp_inline( void (*cb)(void) __attribute__((noreturn)) ) {
  (void)cb;
}

// Control: the same through a typedef.  Clang spells the parameter as the
// typedef name, so the function's type has no "__attribute__" text.  Expect
// "unnecessary_fp_typedef" reported with NO annotation.
typedef void __attribute__((noreturn)) noreturn_fn( void );
static void unnecessary_fp_typedef( noreturn_fn* );
static void unnecessary_fp_typedef( noreturn_fn *cb ) {
  (void)cb;
}

int main( void ) {
  unnecessary_plain( 1 );
  (void)unnecessary_recursive( 3 );
  unnecessary_extern( 2 );
  (void)abs( -1 );
  unnecessary_redundant( 3 );
  necessary_caller();
  necessary_table[0]();
  unnecessary_fp_inline( unnecessary_attr );
  unnecessary_fp_typedef( unnecessary_attr );
  unnecessary_attr();                   // noreturn, so keep it last
}
