#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <ostream>
#include <string>

struct value {
};

using map_type = std::map<std::string,value>;

std::ostream& operator<<( std::ostream &o, map_type::value_type const &e ) {
  return o << e.first;
}

int main() {
  map_type m;
  std::copy(
    m.begin(), m.end(),
    std::ostream_iterator<map_type::value_type>( std::cout, "\n" )
  );
}
