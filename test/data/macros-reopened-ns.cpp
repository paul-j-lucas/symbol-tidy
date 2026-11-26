// Covers macro_Token_getScopedNameCursor() (symbol.c) and, through it,
// getCursorByName_visitor() (clang_util.c) for a *reopened* namespace: "ns" is
// declared in two separate blocks and the macro names a member of the second.
//
// Each USE_* macro is expanded only by "#ifdef", which doesn't reference what
// the macro body names, so a symbol is marked used here only if the macro's
// own tokens resolve to it (see visit_MacroDefinition()).
//
//  USE_A  ns::a   a is in the 1st block
//  USE_B  ns::b   b is in the 2nd block
//
// Expected with a lookup that sees every block of ns:
//
//      unused symbols: "ns::c"
//
// With a lookup that sees only the first block, "ns::b" is also reported
// unused: "ns" resolves to the first block, "b" isn't found in it, and the
// break in macro_Token_getScopedNameCursor() is taken.

namespace ns {
  int a = 1;
}

namespace ns {                          // reopened
  int b = 2;
  int c = 3;                            // control: always reported unused
}

#define USE_A   ns::a
#define USE_B   ns::b

#ifdef USE_A
#endif

#ifdef USE_B
#endif

int main() {
}
