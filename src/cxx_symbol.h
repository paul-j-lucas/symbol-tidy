/*
**      symbol-tidy -- Symbol tidier
**      src/cxx_symbol.h
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

#ifndef cxx_symbol_h
#define cxx_symbol_h

/**
 * @file
 * Defines helper functions for dealing with C++ symbols.
 */

// local
#include "pjl_config.h"
#include "hash_table.h"
#include "symbol.h"

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/**
 * @defgroup tidy-cxx-symbol-group C++ Symbols
 * Helper functions for dealing with C++ symbols.
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Adds the USRs of the user-defined classes (and enumerations) in \a type to
 * \a set, descending through pointers, references, arrays, and, recursively,
 * template arguments.
 *
 * @remarks Classes in standard headers are never added: a user's function
 * can't be found by ADL through `std` (or any other standard namespace) alone.
 *
 * @param type The type to collect from.
 * @param skip_top If `true`, the outermost class itself is not added (but its
 * template arguments are considered).
 * @param set The set to add to.
 */
void cxx_adl_collect_classes( CXType type, bool skip_top, hash_table_t *set );

/**
 * Adds \a name to the global argument-dependent lookup (ADL) name set.
 *
 * @param name The name to add.
 */
void cxx_adl_name_set_add( char const *name );

/**
 * Adds the USRs of user-defined classes appearing as template arguments of
 * \a type to cxx_adl_type_set.  If \a type is a function type, its result and
 * parameter types are considered.
 *
 * @param type The type to collect from.
 */
void cxx_adl_type_set_add( CXType type );

/**
 * A helper function for visit_Constructor() that visits all base classes of \a
 * cursor looking for an explicit call to a base class's constructor: if none
 * is found, look for the base class's default constructor and add a reference
 * to it, if any.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 */
enum CXChildVisitResult cxx_def_ctor_add_ref_visitor( CXCursor cursor,
                                                      CXCursor parent,
                                                      CXClientData data );

/**
 * Helper function for visit_CXXDeleteExpr() that adds a reference to the
 * destructor of the object's class.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
enum CXChildVisitResult cxx_DeleteExpr_visitor( CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData data );

/**
 * Adds references to the destructor of the class of \a decl_csr and all its
 * base class and field destructors.
 *
 * @param decl_csr A field or variable declaration cursor.
 * @param svd The symbols_visit_data to use.
 */
void cxx_dtors_add_refs( CXCursor decl_csr, symbols_visit_data *svd );

/**
 * Adds references from the destructor \a dtor_csr to the destructors of its
 * class's fields and base classes, if any.
 *
 * @remarks A destructor implicitly calls the destructors of its class's fields
 * and base classes, but libclang puts nothing into the AST for those calls.
 * The references must be added from the destructor's own symbol (not from
 * whatever destroys the object) so that they're reachable only if the
 * destructor itself is.
 *
 * @param dtor_csr The destructor's cursor.
 * @param svd The symbols_visit_data to use.
 */
void cxx_dtor_member_add_refs( CXCursor dtor_csr, symbols_visit_data *svd );

/**
 * Adds a reference from a field to its initializer if it's a constructor.
 *
 * @param field_csr The field to add the reference from.
 * @param svd The symbols_visit_data to use.
 */
void cxx_field_def_ctor_add_ref( CXCursor field_csr, symbols_visit_data *svd );

/**
 * A helper function for cxx_field_def_ctor_add_ref().
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns `CXChildVisit_Continue`.
 */
enum CXChildVisitResult cxx_field_def_ctor_add_ref_visitor( CXCursor cursor,
                                                            CXCursor parent,
                                                            CXClientData data );

/**
 * A helper function for visit_CXXForRangeStmt() that attempts to find
 * `begin()` and `end()` functions.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
enum CXChildVisitResult cxx_find_begin_end_visitor( CXCursor cursor,
                                                    CXCursor parent,
                                                    CXClientData data );

/**
 * TODO.
 *
 * @param member_csr The member cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
void cxx_member_add_ref( CXCursor member_csr, symbols_visit_data *svd );

/**
 * Adds a reference from \a member_sym for either a non-static member function
 * or field to the class's default constructor.
 *
 * @remarks
 * @parblock
 * Consider a class and code like:
 *
 *      struct word_info {
 *        word_info() : _count{ 0 } { }
 *
 *        void print() const;
 *
 *        unsigned _count;
 *      };
 *
 *      std::map<std::string,word_info> word_map;
 *      auto &wi = word_map[ "foo" ];
 *
 *      if ( wi._count > 0 )
 *        wi.print();
 *
 * If all accesses to `word_info` objects are via a `map`, the default
 * constructor is only ever called by code in `map`'s implementation and never
 * in user's code (the code being tidied).  Hence, **symbol-tidy** will wrongly
 * conclude that `word_info`'s default constructor is unused.
 *
 * However, if any field is accessed or any member function is called, then
 * it's on an object that _must_ have been constructed.  If the class's only
 * constructor is a default constructor, then _that_ constructor must have been
 * called.
 * @endparblock
 *
 * @param member_csr The cursor for the member.
 * @param member_sym The member symbol to add a reference from.
 * @param svd The symbols_visit_data to use.
 */
void cxx_member_def_ctor_add_ref( CXCursor member_csr, tidy_symbol *member_sym,
                                  symbols_visit_data const *svd );

/**
 * Adds references to the member operators of \a cls_csr named either \a name
 * or \a array_name.  Like C++ name lookup, base classes are searched only if
 * \a cls_csr declares none.
 *
 * @param cls_csr The class's cursor.
 * @param name The operator's name, e.g., `"operator new"`.
 * @param array_name The array operator's name, e.g., `"operator new[]"`.
 * @param svd The symbols_visit_data to use.
 * @return Returns `true` only if at least one reference was added.
 */
PJL_DISCARD
bool cxx_operator_add_refs( CXCursor cls_csr, char const *name,
                            char const *array_name, symbols_visit_data *svd );

/**
 * Initializes the internal set of all C++ symbols.
 *
 * @note This function must be called exactly once.
 */
void cxx_symbols_init( void );

/**
 * Marks as roots all user-defined free functions that template code in
 * standard headers can call via ADL.
 *
 * @remarks
 * @parblock
 * A function is marked only if _both_ are true:
 *
 *  1. Its (unqualified) name was referenced by a dependent call in a standard
 *     header, e.g., `operator<<`.
 *
 *  2. At least one user-defined class in its parameter types is also a
 *     template argument of some type used in user code.
 *
 * Condition 2 is what prevents every user `operator<<` from being marked
 * merely because some standard header shifts a dependent integer.
 * @endparblock
 */
void cxx_symbols_mark_adl_roots( void );

/**
 * Recursively checks the template arguments of a variable's type and adds
 * references to the default constructors of any class arguments.
 *
 * @param type The type to check for template arguments.
 * @param svd The symbols_visit_data to use.
 */
void cxx_var_template_args_def_ctor_add_ref( CXType type,
                                             symbols_visit_data *svd );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* cxx_symbol_h */
/* vim:set et sw=2 ts=2: */
