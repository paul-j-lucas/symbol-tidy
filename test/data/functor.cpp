#include <cstring>
#include <functional>
#include <unordered_set>

size_t hash_string( char const *s ) {
  return 0;
}

namespace std {

template<>
inline size_t hash<char const*>::operator()( char const *s ) const noexcept {
  return hash_string( s );
}

} // namespace

struct char_ptr_equal_to {
  using argument_type = char const*;
  using result_type = bool;

  result_type operator()( argument_type i, argument_type j ) const {
    return std::strcmp( i, j ) == 0;
  }
};

using unordered_char_ptr_set =
  std::unordered_set<char const*,std::hash<char const*>,char_ptr_equal_to>;

int main() {
  char const *const s = "hello, world";
  unordered_char_ptr_set ucps;
  ucps.insert( s );
}
