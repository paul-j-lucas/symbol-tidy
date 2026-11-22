template<typename T>
void f( T x ) {
  (void)x;
}

template<>
void f<long>( long ) {
}

int main() {
  f( 42 );
  f( 42L );
}
