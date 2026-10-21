//
// Tests the special case for enumerations where if at least one of its
// constants is used, consider the enumeration as a whole used even if no code
// uses the name of the enumeration.
//

enum E {
  A
};

int main() {
  int i = A;
}
