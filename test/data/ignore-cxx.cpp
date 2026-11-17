struct S {
  S() { }

  S( S const& ) { }
  S( S&& ) { }

  S& operator=( S const& )  { return *this; }
  S& operator=( S&& )       { return *this; }
};

int main() {
  S s;
}
