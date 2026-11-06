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
 * For C++ only, adds a reference from a field to its initializer if it's a
 * constructor.
 *
 * @param field_csr The field to add the reference from.
 * @param svd The symbols_visit_data to use.
 */
void cxx_field_add_def_ctor_ref( CXCursor field_csr, symbols_visit_data *svd );

/**
 * A helper function for cxx_field_add_def_ctor_ref().
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns `CXChildVisit_Continue`.
 */
enum CXChildVisitResult cxx_field_add_def_ctor_ref_visitor( CXCursor cursor,
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
void cxx_member_add_def_ctor_ref( CXCursor member_csr, tidy_symbol *member_sym,
                                  symbols_visit_data const *svd );

/**
 * TODO.
 *
 * @param member_csr The member cursor to visit.
 */
void cxx_member_add_ref( CXCursor member_csr, symbols_visit_data *svd );

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
 * Recursively checks the template arguments of a variable's type and adds
 * references to the default constructors of any class arguments.
 *
 * @param type The type to check for template arguments.
 * @param svd The symbols_visit_data to use.
 */
void cxx_var_template_args_add_def_ctor_ref( CXType type,
                                             symbols_visit_data *svd );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* cxx_symbol_h */
/* vim:set et sw=2 ts=2: */
