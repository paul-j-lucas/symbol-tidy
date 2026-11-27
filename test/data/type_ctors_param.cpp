// Covers cxx_type_ctors_param() in cxx_symbol.c (via cxx_tmpl_ctors_param()
// from cxx_var_template_args_def_ctor_add_ref()): does a *user* class template
// construct its type parameter?
//
//  by_array    T a[2]             array element type
//  by_typedef  typedef T type;    typedef (and Elaborated in newer clang)
//  by_nested   detail::box<T>     Elaborated; recurses into template args
//  by_other    U u                template param named U, not T: false
//  by_pointer  T*, int            pointer: false; non-template type: false
//
// Each "constructed" template gets its own class so a failure in one branch
// isn't hidden by another.
//
// Expected: only "unconstructed::unconstructed()" is reported unused.

struct w_array       { w_array()       { } };
struct w_typedef     { w_typedef()     { } };
struct w_nested      { w_nested()      { } };
struct unconstructed { unconstructed() { } };   // reported unused

namespace detail {
  template<class T> struct box { T t; };
}

template<class T> struct by_array   { T a[ 2 ]; };
template<class T> struct by_typedef { typedef T type; type t; };
template<class T> struct by_nested  { detail::box<T> b; };
template<class T, class U> struct by_other { U u; };
template<class T> struct by_pointer { T *p = nullptr; int n = 0; };

int main() {
  by_array<w_array>             a;
  by_typedef<w_typedef>         t;
  by_nested<w_nested>           n;
  by_other<unconstructed, int>  o;
  by_pointer<unconstructed>     p;
  return  static_cast<int>(
            sizeof( a.a ) + sizeof( t.t ) + sizeof( n.b.t ) +
            sizeof( by_typedef<w_typedef>::type )
          ) + o.u + p.n + (p.p != nullptr);
}
