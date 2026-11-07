/**
 * Test derived from SWISH++.
 *
 * Copyright (C) 1998-2026 Paul J. Lucas
 *
 * @sa https://github.com/paul-j-lucas/swishxx
 */

#include <vector>

template<typename T>
struct auto_delete_pool {
};

template<typename Derived>
struct auto_delete_obj {
  using pool_object_type = auto_delete_obj<Derived>;
  using pool_type = auto_delete_pool<Derived>;

  explicit auto_delete_obj( pool_type *pool ) : pool_{ pool } {
  }

  explicit auto_delete_obj( pool_type &pool ) : pool_{ &pool } {
  }

  pool_type *const pool_;
};

struct query_node : auto_delete_obj<query_node> {
  query_node() : pool_object_type( nullptr ) { }
  query_node( pool_type &p ) : pool_object_type{ p } { }
};

int main() {
  auto_delete_pool<query_node> pool;
  query_node q{ pool };
  (void)q;
}
