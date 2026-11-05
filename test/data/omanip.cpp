#include "omanip.hpp"

#include <cerrno>
#include <iostream>

inline std::ostream& error_string( std::ostream &o, int err_code ) {
  return o << ": " << std::strerror( err_code ) << std::endl;
}

PJL::omanip<int> error_string( int err_code ) {
  return PJL::omanip<int>( error_string, err_code );
}

int main() {
  std::cout << error_string( ENOENT );
}
