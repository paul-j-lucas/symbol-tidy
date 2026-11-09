#include <ostream>
#include <streambuf>

class counting_buf : public std::streambuf {
protected:
  int_type overflow( int_type c ) override {
    if ( c != traits_type::eof() )
      ++count_;
    return c;
  }

private:
  unsigned count_ = 0;
};

int main() {
  counting_buf buf;
  std::ostream os( &buf );
  os << 'x';
}
