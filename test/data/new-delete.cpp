#include <new>

struct singleton {
  void* operator new( size_t )            { return &_instance; }
  void  operator delete( void*, size_t )  { /* do nothing */ }

  static singleton _instance;
};

singleton singleton::_instance;

int main() {
  singleton *p = new singleton;
  // ...
  delete p;
}
