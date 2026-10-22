struct A {
  A() { }
};

struct B {
  B() { }
};

struct D : B {
  D() { }                               // calls B::B() implicitly
};

int main() {
  A a;                                  // calls A::A() implicitly
  D d;                                  // calls D::D() implicitly
}
