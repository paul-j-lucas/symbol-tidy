/*
**      symbol-tidy -- Symbol tidier
**      src/symbol.h
**
**      Copyright (C) 2026  Paul J. Lucas
**
**      This program is free software: you can redistribute it and/or modify
**      it under the terms of the GNU General Public License as published by
**      the Free Software Foundation, either version 3 of the License, or
**      (at your option) any later version.
**
**      This program is distributed in the hope that it will be useful,
**      but WITHOUT ANY WARRANTY; without even the implied warranty of
**      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**      GNU General Public License for more details.
**
**      You should have received a copy of the GNU General Public License
**      along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef tidy_symbol_h
#define tidy_symbol_h

/**
 * @file
 * Declares structures and functions for keeping track of symbols declared and
 * referenced.
 */

// local
#include "pjl_config.h"
#include "hash_table.h"
#include "source.h"

// standard
#include <stdbool.h>

/**
 * @defgroup tidy-symbols-group Symbols
 * Structures and functions for keeping track of symbols declared and
 * referenced.
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct tidy_symbol tidy_symbol;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A symbol referenced in a translation unit.
 */
struct tidy_symbol {
  char const   *key;                    ///< Unique key.
  char const   *name;                   ///< Name.
  char const   *display_name;           ///< Display name.
  char const   *path;                   ///< File defined in.
  unsigned      line;                   ///< Line defined on.
  unsigned      col;                    ///< Column defined at.
  tidy_symbol  *parent_sym;             ///< Parent symbol, if any.
  hash_table_t  ref_sym_set;            ///< Symbol(s) referenced from this.

  /**
   * Is this a "root" symbol?
   *
   * @remarks
   * @parblock
   * To mark symbols as \ref is_used "used", we have to iterate over
   * all symbols collected from a program looking for "root" symbols.  For each
   * root symbol, mark it as used and also mark every symbol on its \ref
   * ref_sym_set as used.  All symbols not reachable from a root symbol are
   * unused.
   *
   * In C, the only root symbol is `main()`; in C++, the following are also
   * roots:
   *
   *  + All symbols in system headers since template code calls back into user
   *    code, so it should be marked used.
   *
   *  + All variables of types that have constructors.  Even if such variables
   *    are apparently unused, their constructors might have side effects
   *    (e.g., locking a mutex), so they (and any symbols they reference) have
   *    to be marked as used.
   *
   *  + All user-defined `operator()` functions due to limitations of
   *    libclang's C API.
   * @endparblock
   */
  bool          is_root;

  bool          is_dead;                ///< Don't count as unused.
  bool          is_scope;               ///< Is this an enclosing scope?
  bool          is_used;                ///< Is the symbol used?
};

////////// extern functions ///////////////////////////////////////////////////

/**
 * Initializes the internal set of all symbols in the translation unit.
 *
 * @note This function must be called exactly once.
 */
void symbols_init( void );

/**
 * Determines the number of unused symbols and prints them.
 *
 * @return Returns the number of unused symbols.
 */
unsigned symbols_print_unused( void );

/**
 * Visits all symbols in \a source.
 *
 * @param source The source file to visit.
 */
void symbols_visit( tidy_source *source );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_symbol_h */
/* vim:set et sw=2 ts=2: */
