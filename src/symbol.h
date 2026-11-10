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

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/**
 * @defgroup tidy-symbols-group Symbols
 * Structures and functions for keeping track of symbols declared and
 * referenced.
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct cxx_dtor_frame       cxx_dtor_frame;
typedef struct svd_cxx_constructor  svd_cxx_constructor;
typedef struct svd_cxx_for_range    svd_cxx_for_range;
typedef struct svd_cxx_member       svd_cxx_member;
typedef struct svd_cxx_operator     svd_cxx_operator;
typedef struct symbols_visit_data   symbols_visit_data;
typedef struct tidy_loc             tidy_loc;
typedef struct tidy_symbol          tidy_symbol;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A source file location.
 */
struct tidy_loc {
  char const   *path;                   ///< File defined in.
  unsigned      line;                   ///< Line defined on.
  unsigned      col;                    ///< Column defined at.
};

/**
 * A symbol referenced in a translation unit.
 */
struct tidy_symbol {
  char const   *key;                    ///< Unique key.
  char const   *name;                   ///< Name.
  char const   *cxx_prototype;          ///< C++ prototype.
  tidy_loc      loc;                    ///< Location.
  tidy_loc      macro_loc;              ///< Location if defined in a macro.
  tidy_symbol  *parent_sym;             ///< Parent symbol, if any.
  hash_table_t  ref_sym_set;            ///< Symbol(s) referenced from this.

  /**
   * For C++ functions only, USRs of user-defined classes and enumerations that
   * appear in the parameter types of this function.
   *
   * @remarks
   * @parblock
   * Templates in standard headers can call a user's functions that are found
   * only by argument-dependent lookup (ADL) at the point of instantiation,
   * e.g., a user-defined `operator<<` called from within
   * `std::ostream_iterator<T>::operator=()`.
   *
   * Libclang's C API neither visits template instantiations nor records ADL
   * candidates for dependent calls, so no reference to such a function is ever
   * seen.
   *
   * This set and the global sets in `symbol.c` are used to decide whether such
   * a function can be found by ADL from standard template code, and, if so, to
   * mark it as a \ref is_root "root".
   *
   * Unlike other hash tables used in **symbol-tidy**, this one is lazily
   * initialized: it's empty (and allocates nothing) for every symbol that
   * isn't a C++ template function with at least one user-defined class among
   * its parameter types.
   * @endparblock
   *
   * @sa https://en.cppreference.com/cpp/language/adl
   * @sa https://en.wikipedia.org/wiki/Argument-dependent_name_lookup
   */
  hash_table_t  cxx_adl_usr_set;

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
   *    code, so they should be marked as roots.
   *
   *  + All variables of types that have constructors.  Even if such variables
   *    are apparently unused, their constructors might have side effects
   *    (e.g., locking a mutex), so they have to be marked as a root.
   *
   *  + All user-defined `operator()` functions due to limitations of
   *    libclang's C API.
   * @endparblock
   */
  bool          is_root;

  /**
   * Is this symbol "dead"?
   *
   * @remarks
   * @parblock
   * A "dead" symbol is one that is excluded from being considered unused (even
   * if it is unused).  A symbol becomes dead if any of the following are true:
   *
   *  1. Its \ref path is NULL or its \ref line is 0 (as they would be for
   *     symbols in system headers).
   *
   *  2. It's within an unused scope.
   * @endparblock
   *
   * @sa symbol_in_unused_scope()
   */
  bool          is_dead;

  /**
   * Is this symbol a scope?
   *
   * @remarks
   * Consider a class:
   *
   *      struct point {
   *        point() : x{ 0 }, y{ 0 } { }
   *        int x, y;
   *      };
   *
   * It has the symbols `point`, `point::point()`, `point::x`, and `point::y`.
   * If all are unused (meaning the entire `struct` is unused), it's much less
   * verbose and necessary to report only that `point` (the `struct`) is unused
   * rather than reporting that every symbol "in" `point` is unused also.
   *
   * @sa symbol_in_unused_scope()
   * @sa tidy_Cursor_isEnclosingScope()
   */
  bool          is_scope;

  bool          is_used;                ///< Is this symbol used?
};

/**
 * Additional data passed to symbols_visitor().
 */
struct symbols_visit_data {
  tidy_source const  *source;           ///< Source file being tidied.
  tidy_symbol        *curr_sym;         ///< Current symbol, if any.

  /**
   * Mini file_previously_seen() result cache that allows cursors from header
   * files we've already visited to be skipped.
   */
  struct {
    CXFile            last_file;        ///< The file for the last cursor.
    bool              last_seen;        ///< Saw \ref last_file?
  } fps;

  /**
   * Additional data for visit_Constructor().
   */
  struct svd_cxx_constructor {
    CXCursor          ctor_csr;         ///< Constructor's cursor.
  } cxx_constructor;

  /**
   * Additional data for cxx_dtor_add_ref().
   */
  struct svd_cxx_dtor {
    cxx_dtor_frame const *top;          ///< The top destructor frame.
  } cxx_dtor;

  /**
   * Additional data for visit_CXXForRangeStmt().
   */
  struct svd_cxx_for_range {
    CXCursor          cls_csr;          ///< Class being iterated over.
    CXCursor          begin_csr;        ///< Found `begin()` cursor, if any.
    CXCursor          end_csr;          ///< Found `end()` cursor, if any.
    bool              is_const;         ///< Is the range-for `const`?
  } cxx_for_range;

  /**
   * Additional data for cxx_member_add_refs_by_name().
   */
  struct svd_cxx_member {
    char const       *name;             ///< Member name to match.
    bool              found;            ///< Found at least one member?
  } cxx_member;

  /**
   * Additional data for cxx_operator_add_refs().
   */
  struct svd_cxx_operator {
    char const       *name;             ///< Name, e.g., `operator new`.
    char const       *array_name;       ///< Array name, e.g., `operator new[]`.
    bool              found;            ///< Found at least one operator?
  } cxx_operator;
};

////////// extern functions ///////////////////////////////////////////////////

/**
 * Adds a reference from \ref symbols_visit_data::curr_sym "curr_sym" to the
 * symbol for \a to_csr, or mark the latter as a \ref tidy_symbol::is_root
 * "root".
 *
 * @param to_csr The cursor whose symbol to add a reference to or be marked as
 * a \ref tidy_symbol::is_root "root".
 * @param svd The symbols_visit_data to use.
 *
 * @sa symbol_add_ref()
 */
void cursor_add_ref( CXCursor to_csr, symbols_visit_data *svd );

/**
 * Adds a symbol to tidy_symbol_set unless it already exists.
 *
 * @param sym_csr The cursor for the symbol to add.
 * @param source The tidy_source \a sym_csr is from.
 * @return Returns a new or existing tidy_symbol, or NULL if it is excluded or
 * could could not be added.
 */
NODISCARD
tidy_symbol* symbol_add( CXCursor sym_csr, tidy_source const *source );

/**
 * Adds a reference from \a from_sym to \a to_sym, or mark \a to_sym as a \ref
 * tidy_symbol::is_root "root".
 *
 * @param from_sym The symbol to add a reference from. If NULL, \a to_sym is
 * marked as a \ref tidy_symbol::is_root "root".
 * @param to_sym The symbol to add a reference to or be marked as a \ref
 * tidy_symbol::is_root "root".
 *
 * @sa cursor_add_ref()
 */
void symbol_add_ref( tidy_symbol *from_sym, tidy_symbol *to_sym );

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

////////// extern variables ///////////////////////////////////////////////////

extern hash_table_t tidy_symbol_set;    ///< Set of symbols.

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_symbol_h */
/* vim:set et sw=2 ts=2: */
