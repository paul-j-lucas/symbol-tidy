struct A {
  A() { }
};

struct B {
  B() { }
};

struct D : B {
  D() { }                               // calls B::B() implicitly
};

struct class_with_no_constructor {
};

int main() {
  A a_obj;                              // calls A::A() implicitly
  A a_array[10];                        // calls A::A() for each element
  D d_obj;                              // calls D::D() implicitly
  class_with_no_constructor obj_with_no_constructor;
}
