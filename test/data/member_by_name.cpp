// Covers cxx_member_by_name_base_visitor() in cxx_symbol.c (reached via
// cxx_member_add_refs_by_name() when a dependent member access finds nothing
// in the class itself).
//
//  + pool_->add( 1 ): "add" isn't in pool<T> or mid, only in base, so the
//    visitor recurses pool<T> -> mid -> base.
//  + pt_->add( 2 ): pool_t<T>'s only base is the parameter T, so
//    tidy_Cursor_getBaseSpecifierClass() is invalid and the visitor skips it.
//
// Expected: base::unused_in_base() and mid::other() are reported unused;
// base::add() and holder::add() are not.

struct base {
  void add( int ) { }
  void unused_in_base() { }             // reported unused
};

struct mid : base {
  void other() { }                      // reported unused
};

template<class T> struct pool   : mid { };
template<class T> struct pool_t : T   { };

struct holder {
  void add( int ) { }
};

template<class T>
struct user {
  pool<T>   *pool_ = nullptr;
  pool_t<T> *pt_   = nullptr;

  void f() {
    pool_->add( 1 );
    pt_->add( 2 );
  }
};

int main() {
  user<holder> u;
  u.f();
  holder h;
  h.add( 0 );
}
