struct A {
  ~A() { }
};

template<typename T>
struct B {
  A a;
};

int main() {
  B<int> b;
  (void)b;
}
