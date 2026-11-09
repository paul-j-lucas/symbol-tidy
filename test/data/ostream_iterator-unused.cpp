#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>

namespace ns {

struct point { int x; };
struct other { int y; };

std::ostream& operator<<( std::ostream &os, point const &p ) {
  return os << p.x;
}

std::ostream& operator<<( std::ostream &os, other const &o ) {
  return os << o.y;
}

} // namespace ns

int main() {
  std::vector<ns::point> v{ {1}, {2} };
  std::copy(
    v.begin(), v.end(), std::ostream_iterator<ns::point>( std::cout, " " )
  );
}
