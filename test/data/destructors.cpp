struct A {
  ~A() { }
};

struct B {
  ~B() { }
};

struct D : B {
  ~D() { }
};

int main() {
  D d;
  (void)d;

  A *p = new A{};
  delete p;
}
