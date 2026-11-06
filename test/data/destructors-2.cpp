struct A {
  ~A() { }
};

struct B : A {
  ~B() { }
};

struct C : B {
  ~C() { }
};

int main() {
  C *p = new C;
  delete p;
}
