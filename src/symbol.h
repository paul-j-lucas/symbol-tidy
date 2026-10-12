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

/**
 * @defgroup tidy-symbols-group Symbols
 * Structures and functions for keeping track of symbols declared and
 * referenced.
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct tidy_ref     tidy_ref;
typedef struct tidy_symbol  tidy_symbol;

////////// structs ////////////////////////////////////////////////////////////

/**
 * TODO
 */
struct tidy_ref {
  tidy_symbol *sym;
  tidy_ref    *next;
};

/**
 * A symbol referenced in a translation unit.
 */
struct tidy_symbol {
  char const *key;                      ///< Unique key.
  char const *name;                     ///< Name.
  char const *file;
  unsigned    line;
  unsigned    col;
  bool        is_root;        /* true if 'main' or referenced from global scope */
  bool        is_reachable;             ///< Reachable from a root symbol?
  tidy_ref   *refs;
};

////////// extern functions ///////////////////////////////////////////////////

/**
 * Compares two \ref tidy_symbol objects.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the name of \a
 * i_sym is less than, equal to, or greater than the name of \a j_sym,
 * respectively.
 */
NODISCARD
int symbol_cmp( tidy_symbol const *i_sym, tidy_symbol const *j_sym );

/**
 * Calculates the hash of \a sym.
 *
 * @param sym The tidy_symbol to calculate the hash for.
 * @return Returns said hash.
 */
NODISCARD
ht_hash_val_t symbol_hash( tidy_symbol const *sym );

/**
 * Initializes the internal set of all symbols in the translation unit.
 *
 * @note This function must be called exactly once.
 */
void symbols_init( void );

/**
 * TODO.
 *
 * @param source TODO.
 */
void symbols_visit( tidy_source *source );

/**
 * TODO.
 *
 * @return Returns TODO.
 */
unsigned symbols_report_unused( void );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_symbol_h */
/* vim:set et sw=2 ts=2: */
