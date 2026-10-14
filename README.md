# symbol-tidy

THIS SOFTWARE IS PRE-ALPHA AND DOESN'T WORK YET.
STAY TUNED.

## Introduction

Most C and C++ compilers can warn only when a parameter
or `static` variable or function
is unused
within a single source file.

**symbol-tidy** (Tidy) is a command-line tool
that allows you to tidy-up symbols
in a set of either C or C++ source files
comprising a program,
specifically:

+ For every symbol (macro, type, non-`static` variable or function)
  in a set of source files, it should be used at least once.

That means Tidy will print a warning for those that aren't.

**symbol-tidy** has a companion
[**include-tidy**](https://github.com/paul-j-lucas/include-tidy/)
project.

## Dependencies

Tidy has the following dependencies:

+ [Libclang](https://clang.llvm.org/docs/LibClang.html)
  (typically installed as part of either a `clang`
  or `clang-devel`
  package,
  if not from source).

+ The [`llvm-config`](https://llvm.org/docs/CommandGuide/llvm-config.html)
  command
  (typically installed as part of either an `llvm`
  or `llvm-devel`
  package,
  if not from source).

+ The [clang](https://clang.llvm.org)
  or [gcc](https://gcc.gnu.org)
  compiler.

Although Clang and LLVM are often packaged seperately,
their versions increment in lockstep,
so you _should_ install the same version for both.

Tidy was written against version 21.x of Libclang.
(Hopefully, Tidy will continue to work with future versions.)
Tidy is known _not_ to work correctly
with version 18.
Nothing is known about either versions 19 or 20.

## Installation

The git repository contains only the necessary source code.
Things like `configure` are _derived_ sources and
[should not be included in repositories](http://stackoverflow.com/a/18732931).
If you have
[`autoconf`](https://www.gnu.org/software/autoconf/),
[`automake`](https://www.gnu.org/software/automake/),
and
[`m4`](https://www.gnu.org/software/m4/)
installed,
you can generate `configure` yourself by doing:

    ./bootstrap

Or you can download a
[released version](https://github.com/paul-j-lucas/symbol-tidy/releases)
that contains `configure`.
In either case,
then follow the generic installation instructions
given in `INSTALL`.

If you would like to generate the developer documentation,
you will also need
[Doxygen](http://www.doxygen.org/);
then do:

    make doc                            # or: make docs

Since Tidy uses
[Autotools](https://en.wikipedia.org/wiki/GNU_Autotools)
that is exclusively for Unix-like environments,
Tidy
(or any other software that uses Autotools)
will not compile
nor run
on native Windows.
If you're determined to run Tidy on Windows,
you might try one of
[Cygwin](https://www.cygwin.com),
[MinGW-64](https://www.mingw-w64.org),
or
[WSL](https://learn.microsoft.com/en-us/windows/wsl/).

**Paul J. Lucas**  
San Francisco Bay Area, California, USA  
1 October 2026
