/**
 * Test derived from SWISH++.
 *
 * Copyright (C) 1998-2026 Paul J. Lucas
 *
 * @sa https://github.com/paul-j-lucas/swishxx
 */

#ifndef swishxx_omanip_H
#define swishxx_omanip_H

#include <ostream>

namespace PJL {

template<typename Arg>
class omanip {
public:
  using func_type = std::ostream& (*)( std::ostream&, Arg );

  omanip( func_type f, Arg const &arg ) : f_{ f }, arg_{ arg } { }

  friend std::ostream& operator<<( std::ostream &o, omanip<Arg> const &om ) {
    return (*om.f_)( o, om.arg_ );
  }

private:
  func_type const f_;
  Arg const arg_;
};

} // namespace PJL

#endif /* swishxx_omanip_H */
/* vim:set et sw=2 ts=2: */
