// Covers the recursion guard (depth > 16) in cxx_type_ctors_param().
//
// cxx_type_ctors_param() recurses once per level of template-argument nesting,
// so for nest<nest<...<T>...>> with k levels, T is reached at depth k:
//
//  deep16<w_ok>    16 levels: T at depth 16, allowed: w_ok's ctor is referenced
//  deep17<w_deep>  17 levels: T at depth 17, guard returns false: w_deep's ctor
//                  is not referenced (a deliberate limit)
//
// The 16-level case is the control: it shows that "not reported" vs. "reported"
// is due to the guard and not to nest<> never constructing T.
//
// Expected: only "w_deep::w_deep()" is reported unused.

struct w_ok   { w_ok()   { } };
struct w_deep { w_deep() { } };         // reported unused

template<class T> struct nest { T t; };

#define N1( X )   nest<X>
#define N4( X )   N1( N1( N1( N1( X ) ) ) )
#define N16( X )  N4( N4( N4( N4( X ) ) ) )

template<class T> struct deep16 { N16( T )        d; };         // 16 levels
template<class T> struct deep17 { N16( nest<T> )  d; };         // 17 levels

int main() {
  deep16<w_ok>    a;
  deep17<w_deep>  b;
  return static_cast<int>( sizeof( a.d.t ) + sizeof( b.d.t ) );
}
