// Covers macro_Token_getScopedNameCursor() in symbol.c: C++ "::"-scoped names
// inside macro bodies.
//
// Expected: only ns::unused_var is reported unused.

namespace ns {
  int used_var   = 1;
  int unused_var = 2;                   // reported unused

  namespace inner {
    inline int g() { return 3; }
  }

  struct S { static int sm; };
  int S::sm = 4;
}

#define SCOPED          ns::used_var    // "ns" "::" "used_var"
#define NESTED()        ns::inner::g()  // two trips through the loop
#define STATIC_MEMBER   ns::S::sm       // class scope
#define ONLY_NS         ns              // tidy_Token_getNext() == NULL
#define SIZE            sizeof( ns::S ) // next token isn't "::"
#define IN_NS( name )   ns::name        // "name" is a macro parameter

namespace alias = ONLY_NS;

int main() {
  return SCOPED + NESTED() + STATIC_MEMBER + (int)SIZE + IN_NS( used_var );
}
