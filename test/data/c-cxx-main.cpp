extern "C"  void c_msg( void );
extern      void cxx_msg( void );

int main() {
  c_msg();
  cxx_msg();
}
