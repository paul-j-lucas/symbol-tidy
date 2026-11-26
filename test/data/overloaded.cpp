// Covers visit_OverloadedDeclRef() in symbol.c.
//
// Two sources of CXCursor_OverloadedDeclRef:
//   1. a using-declaration that names more than one overload;
//   2. an unqualified call with a dependent argument inside a template, which
//      libclang can't resolve until instantiation.
//
// Expected: only ns::unused_in_ns() is reported unused.

namespace ns {
  void f( int )    { }
  void f( double ) { }
  void unused_in_ns() { }               // reported unused
}

using ns::f;                            // (1) both f's

void h( int )          { }
void h( char const* )  { }

template<class T>
void call_h( T t ) {
  h( t );                               // (2) both h's; dependent call
}

int main() {
  f( 1 );
  f( 2.0 );
  call_h( 3 );
}
