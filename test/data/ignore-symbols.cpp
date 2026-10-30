typedef char* pointer;
typedef char const* const_pointer;

typedef char& reference;
typedef char const& const_reference;

struct S {
  typedef char* pointer;
  typedef char const* const_pointer;
  typedef char& reference;
  typedef char const& const_reference;

  S() { }
};

int main() {
  S s;
  char c;

  reference r = c;
  (void)r;

  const_reference cr = c;
  (void)cr;
}
