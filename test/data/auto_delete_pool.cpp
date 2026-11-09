/**
 * Test derived from SWISH++.
 *
 * Copyright (C) 1998-2026 Paul J. Lucas
 *
 * @sa https://github.com/paul-j-lucas/swishxx
 */

template<typename T>
struct auto_delete_pool {
  ~auto_delete_pool() {
  }

  void add( void *obj ) {
    (void)obj;
  }
};

template<typename Derived>
struct auto_delete_obj {
  using pool_object_type = auto_delete_obj<Derived>;
  using pool_type = auto_delete_pool<Derived>;

  virtual ~auto_delete_obj() {
  }

  explicit auto_delete_obj( pool_type &pool ) : pool_{ &pool } {
    pool_->add( this );
  }

  pool_type *const pool_;
};

struct query_node : auto_delete_obj<query_node> {
  query_node( pool_type &p ) : pool_object_type{ p } { }
};

struct and_node : query_node {
  and_node( pool_type &p ) : query_node{ p } { }
};

int main() {
  and_node::pool_type pool;
  query_node *n = new and_node( pool );
  (void)n->pool_;
  delete n;
}
