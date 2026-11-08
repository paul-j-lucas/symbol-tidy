/*
**      symbol-tidy -- Symbol tidier
**      src/cxx_symbol.c
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

/**
 * @file
 * Declares helper functions for dealing with C++ symbols.
 */

// local
#include "pjl_config.h"
#include "cxx_symbol.h"
#include "cxx_util.h"
#include "clang_util.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "source.h"
#include "symbol.h"
#include "symbol-tidy.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup tidy-cxx-symbol-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct cxx_tmpl_ctors_visitor_data  cxx_tmpl_ctors_visitor_data;
typedef struct cxx_tmpl_param_visitor_data  cxx_tmpl_param_visitor_data;

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data for cxx_dtor_add_ref().
 */
struct cxx_dtor_frame {
  CXCursor              cls_csr;        ///< Class with implicit destructor.
  cxx_dtor_frame const *prev_frame;     ///< The previous frame, if any.
};

/**
 * Additional data for cxx_tmpl_ctors_visitor().
 */
struct cxx_tmpl_ctors_visitor_data {
  char const *find_param_name;          ///< Parameter name to find.
  bool        is_param_constructed;     ///< Is the parameter constructed?
};

/**
 * Additional data for cxx_tmpl_param_visitor().
 */
struct cxx_tmpl_param_visitor_data {
  unsigned  find_idx;                   ///< Index of parameter to find.
  unsigned  curr_idx;                   ///< Current index.
  CXCursor  found_csr;                  ///< Found template type cursor.
  bool      found_tmpl_type_param;      ///< Found it?
};

////////// local functions ////////////////////////////////////////////////////

static void cxx_dtor_add_ref( CXCursor, symbols_visit_data* );

static enum CXChildVisitResult
            cxx_field_dtor_add_ref_visitor( CXCursor, CXCursor, CXClientData ),
            cxx_member_add_refs_by_name_visitor( CXCursor, CXCursor,
                                                 CXClientData ),
            cxx_member_by_name_base_visitor( CXCursor, CXCursor, CXClientData ),
            cxx_tmpl_ctors_visitor( CXCursor, CXCursor, CXClientData ),
            cxx_tmpl_param_visitor( CXCursor, CXCursor, CXClientData );

NODISCARD
static bool cxx_type_ctors_param( CXType, char const*, unsigned );

static void cxx_type_template_args_dtor_add_ref( CXType, symbols_visit_data* );
static void print_statistics( void );
static void str_set_insert( hash_table_t*, char const* );

////////// local variables ////////////////////////////////////////////////////

/**
 * Set of names of functions referenced via an `OverloadedDeclRef` from within
 * standard headers, e.g., `operator<<`.  Such a reference is a dependent call
 * that may find user-defined functions by ADL when instantiated.
 *
 * @remarks Lazily initialized.  Entries are strings stored internally.
 */
static hash_table_t cxx_adl_name_set;

/**
 * Set of USRs of user-defined classes and enumerations that appear, at any
 * depth, as template arguments of types seen in user code.  These are the
 * classes that can make a user's function visible to ADL from within an
 * instantiation of a standard template.
 *
 * @remarks Lazily initialized.  Entries are strings stored internally.
 */
static hash_table_t cxx_adl_type_set;

////////// local functions ////////////////////////////////////////////////////

/**
 * A helper function for visit_VarDecl() that adds a reference to each base
 * class's destructor.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns either `CXChildVisit_Continue` or `CXChildVisit_Recurse`.
 */
static
enum CXChildVisitResult cxx_base_dtor_add_ref_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_CXXBaseSpecifier )
    goto skip;

  CXCursor base_cls_csr = tidy_Cursor_getBaseSpecifierClass( cursor );
  if ( tidy_Cursor_isInvalid( base_cls_csr ) ) {
    CXType const type = clang_getCursorType( cursor );
    base_cls_csr = clang_getTypeDeclaration( type );
  }
  base_cls_csr = clang_getCanonicalCursor( base_cls_csr );
  if ( tidy_Cursor_isInvalid( base_cls_csr ) )
    goto skip;

  assert( data != NULL );
  symbols_visit_data *const svd = data;
  cxx_dtor_add_ref( base_cls_csr, svd );
  return CXChildVisit_Recurse;

skip:
  return CXChildVisit_Continue;
}

/**
 * Adds a reference to the destructor of \a cls_csr, if any.
 *
 * @param cls_csr The cursor for the class.
 * @param svd The symbols_visit_data to use.
 */
static void cxx_dtor_add_ref( CXCursor cls_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor const dtor_csr = tidy_Cursor_getDestructor( cls_csr );
  if ( !tidy_Cursor_isInvalid( dtor_csr ) ) {
    //
    // The class has an explicit destructor, so add a reference to it and we're
    // done.
    //
    cursor_add_ref( dtor_csr, svd );
    return;
  }

  //
  // If we've been called on this cls_csr before, bail out.  Given:
  //
  //      struct Tree {
  //        // ...
  //        std::unique_ptr<Tree> left, right;
  //      };
  //
  //      int main() {
  //        Tree t;
  //      }
  //
  // Note that Tree has no used-defined explicit destructor, so the compiler
  // will synthesize an implicit default constructor.  When `Tree t` is
  // visited, the following happend:
  //
  //  1. cxx_dtor_add_ref( Tree ) is called. Since Tree has no explicit
  //     destructor, tidy_Cursor_getDestructor() returns an invalid cursor.
  //     The frame list is empty, so a frame F1 { Tree } is pushed on the stack
  //     and top = &F1.
  //
  //  2. The cxx_field_dtor_add_ref_visitor() visits `left`.  The type
  //     unique_ptr<Tree> has an explicit destructor, so cxx_dtor_add_ref()
  //     adds a reference to it and returns.
  //
  //  3. Next, cxx_type_template_args_dtor_add_ref( unique_ptr<Tree> ) is
  //     called by the visitor. Its argument is Tree, so it calls
  //     cxx_dtor_add_ref( Tree ) again.
  //
  //  4. Since Tree has no explicit destructor, the code walks top -> F1, finds
  //     clang_equalCursors( F1.cls_csr, Tree ) is true, and returns.  Without
  //     the frame, this step would re-push Tree and repeat steps 2–3 until the
  //     stack overflowed.
  //
  cls_csr = clang_getCanonicalCursor( cls_csr );
  for ( cxx_dtor_frame const *frame = svd->cxx_dtor.top; frame != NULL;
        frame = frame->prev_frame ) {
    if ( clang_equalCursors( frame->cls_csr, cls_csr ) )
      return;
  } // for

  cxx_dtor_frame const frame = {
    .cls_csr = cls_csr,
    .prev_frame = svd->cxx_dtor.top
  };
  svd->cxx_dtor.top = &frame;

  // Implicit destructor calls field and base destructors.
  tidy_visitClassChildren( cls_csr, &cxx_field_dtor_add_ref_visitor, svd );
  tidy_visitClassChildren( cls_csr, &cxx_base_dtor_add_ref_visitor, svd );

  //
  // An implicit instantiation exposes no children, so also visit the template
  // it was instantiated from.  Fields and bases whose types are concrete
  // (e.g., Foo f;) resolve there; those that depend on a template parameter
  // (e.g., T f;) are skipped since they aren't class declarations.
  //
  CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( cls_csr );
  if ( !tidy_Cursor_isInvalid( tmpl_csr ) ) {
    tidy_visitClassChildren( tmpl_csr, &cxx_field_dtor_add_ref_visitor, svd );
    tidy_visitClassChildren( tmpl_csr, &cxx_base_dtor_add_ref_visitor, svd );
  }

  svd->cxx_dtor.top = frame.prev_frame;
}

/**
 * A helper function for cxx_dtors_add_refs().
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns `CXChildVisit_Continue`.
 */
static enum
CXChildVisitResult cxx_field_dtor_add_ref_visitor( CXCursor cursor,
                                                   CXCursor parent,
                                                   CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind == CXCursor_FieldDecl ) {
    CXCursor const cls_csr = tidy_Cursor_getUnderlyingTypeDecl( cursor );
    if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
      symbols_visit_data *const svd = data;
      cxx_dtor_add_ref( cls_csr, svd );
      CXType  type = clang_getCursorType( cursor );
              type = tidy_Type_getUnderlyingType( type );
      cxx_type_template_args_dtor_add_ref( type, svd );
    }
  }
  return CXChildVisit_Continue;
}

/**
 * Adds a reference to every member of \a cls_csr whose name is \a name; or, if
 * there are none, then of its base classes.
 *
 * @remarks This is for a member access whose base expression has a dependent
 * type, such as `pool_->add( this )` inside a class template.  libclang can't
 * resolve such an access, so we have to match by name.  All overloads are
 * referenced since the argument types can't be resolved either.
 *
 * @param cls_csr The cursor of the class to search.
 * @param name The member's name.
 * @param svd The symbols_visit_data to use.
 * @return Returns `true` only if at least one member having \a name was found.
 */
PJL_DISCARD
static bool cxx_member_add_refs_by_name( CXCursor cls_csr, char const *name,
                                         symbols_visit_data *svd ) {
  assert( name != NULL );
  assert( svd != NULL );

  if ( tidy_Cursor_isInvalid( cls_csr ) || name[0] == '\0' )
    return false;

  svd_cxx_member const prev_svd_cxx_member = svd->cxx_member;
  svd->cxx_member = (svd_cxx_member){ .name = name };

  tidy_visitClassChildren( cls_csr, &cxx_member_add_refs_by_name_visitor, svd );

  //
  // If cls_csr is an implicit instantiation, libclang doesn't expose its
  // members, so look in the template it was instantiated from.
  //
  if ( !svd->cxx_member.found ) {
    CXCursor tmpl_csr = clang_getSpecializedCursorTemplate( cls_csr );
    if ( !tidy_Cursor_isInvalid( tmpl_csr ) ) {
      tidy_visitClassChildren(
        tmpl_csr, &cxx_member_add_refs_by_name_visitor, svd
      );
    }
  }

  //
  // Per C++ name hiding, a member found in the class hides the same name in
  // its bases, so look in the bases only if nothing was found.
  //
  if ( !svd->cxx_member.found )
    tidy_visitClassChildren( cls_csr, &cxx_member_by_name_base_visitor, svd );

  bool const found = svd->cxx_member.found;
  svd->cxx_member = prev_svd_cxx_member;
  return found;
}


/**
 * Helper function for cxx_member_add_refs_by_name() that adds a reference to
 * every data member, member function, member function template, or static
 * data member whose name matches.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns `CXChildVisit_Continue`.
 */
static enum
CXChildVisitResult cxx_member_add_refs_by_name_visitor( CXCursor cursor,
                                                        CXCursor parent,
                                                        CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_CXXMethod:
    case CXCursor_FieldDecl:
    case CXCursor_FunctionTemplate:
    case CXCursor_VarDecl:              // static data member
      assert( data != NULL );
      symbols_visit_data *const svd = data;
      if ( tidy_Cursor_isSpellingEqualTo( cursor, svd->cxx_member.name ) ) {
        cursor_add_ref( cursor, svd );
        svd->cxx_member.found = true;
      }
      break;
    default:
      /* suppress warning */;
  } // switch

  return CXChildVisit_Continue;
}

/**
 * Helper function for cxx_member_add_refs_by_name() that recurses into each
 * base class.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Always returns `CXChildVisit_Continue`.
 */
static enum
CXChildVisitResult cxx_member_by_name_base_visitor( CXCursor cursor,
                                                    CXCursor parent,
                                                    CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  if ( clang_getCursorKind( cursor ) != CXCursor_CXXBaseSpecifier )
    goto skip;

  CXCursor const base_csr = tidy_Cursor_getBaseSpecifierClass( cursor );
  if ( tidy_Cursor_isInvalid( base_csr ) )
    goto skip;                          // e.g., "struct S : T" with T a param

  //
  // Copy the name first since the recursive call replaces (then restores)
  // svd->cxx_member.
  //
  char const *const name = svd->cxx_member.name;
  if ( cxx_member_add_refs_by_name( base_csr, name, svd ) )
    svd->cxx_member.found = true;

skip:
  return CXChildVisit_Continue;
}

/**
 * Helper function for cxx_operator_add_refs() that adds a reference to every
 * member operator whose name matches.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Always returns `CXChildVisit_Continue`.
 */
static
enum CXChildVisitResult cxx_operator_add_refs_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  switch ( clang_getCursorKind( cursor ) ) {
    case CXCursor_CXXMethod:
    case CXCursor_FunctionTemplate:;
      int const found = tidy_Cursor_isSpellingEqualToAny(
        cursor, svd->cxx_operator.name, svd->cxx_operator.array_name
      );
      if ( found >= 0 ) {
        cursor_add_ref( cursor, svd );
        svd->cxx_operator.found = true;
      }
      break;
    default:
      /* suppress warning */;
  } // switch

  return CXChildVisit_Continue;
}

/**
 * Helper function for cxx_operator_add_refs() that recurses into each
 * base class.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Always returns `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult cxx_operator_base_visitor( CXCursor cursor,
                                                          CXCursor parent,
                                                          CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_CXXBaseSpecifier )
    goto skip;

  CXCursor const base_csr = tidy_Cursor_getBaseSpecifierClass( cursor );
  if ( tidy_Cursor_isInvalid( base_csr ) )
    goto skip;

  //
  // Copy the names first since the recursive call replaces (then restores)
  // svd->cxx_operator.
  //
  char const *const name = svd->cxx_operator.name;
  char const *const array_name = svd->cxx_operator.array_name;
  if ( cxx_operator_add_refs( base_csr, name, array_name, svd ) )
    svd->cxx_operator.found = true;

skip:
  return CXChildVisit_Continue;
}

/**
 * Cleans-up all C++ symbols.
 */
static void cxx_symbols_cleanup( void ) {
  print_statistics();
  ht_table_cleanup( &cxx_adl_name_set, /*free_fn=*/NULL );
  ht_table_cleanup( &cxx_adl_type_set, /*free_fn=*/NULL );
}

/**
 * Gets whether the class template that \a spec_csr is a specialization of
 * might construct a `T` for its \a param_idx-th template parameter.
 *
 * @remarks
 * @parblock
 * Libclang neither visits template instantiations nor resolves dependent
 * expressions, so whether a template constructs its parameter can't be
 * observed directly.  Instead, this function decides as follows:
 *
 *  + If the template is declared in a system header, its code is invisible to
 *    us and (as for `std::vector<T>`) it's assumed to construct its parameter.
 *
 *  + Otherwise, the template's own declaration is scanned for evidence that it
 *    constructs its corresponding template type parameter, `T`: a variable or
 *    field of type `T`, an array of `T`, a `T` base class, `T()` or `T{}`,
 *    `new T`, or a type that has `T` as a template parameter (e.g.,
 *    `std::vector<T>`).
 *
 *  + If it can't be determined (because \a spec_csr isn't a specialization,
 *    the template is a partial specialization, or the argument corresponds to
 *    a parameter pack, a non-type parameter, or a template template
 *    parameter), it's assumed that the template does construct its argument.
 *    This errs on the side of not reporting a false positive.
 *
 * This is used by cxx_var_template_args_def_ctor_add_ref() to decide whether
 * to add a reference to the default constructor of a class used as a template
 * parameter.
 * @endparblock
 *
 * @param spec_csr The cursor for the template specialization.
 * @param param_idx The zero-based index of the template parameter
 * corresponding to the argument of \a spec_csr to check.
 * @return Returns `true` if the template is in a system header (its code is
 * invisible to libclang), if we can't tell (to err on the side of not
 * reporting a false positive), or if the template's own code shows evidence of
 * constructing its argument; `false` only if user code shows no evidence.
 *
 * @sa cxx_var_template_args_def_ctor_add_ref()
 */
NODISCARD
static bool cxx_tmpl_ctors_param( CXCursor spec_csr, unsigned param_idx ) {
  CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( spec_csr );
  if ( tidy_Cursor_isInvalid( tmpl_csr ) )
    return true;
  if ( tidy_Cursor_isInSystemHeader( tmpl_csr ) )
    return true;
  enum CXCursorKind kind = clang_getCursorKind( tmpl_csr );
  if ( kind != CXCursor_ClassTemplate )
    return true;                        // partial specialization: can't tell

  cxx_tmpl_param_visitor_data tpvd = { .find_idx = param_idx };
  tidy_visitClassChildren( tmpl_csr, &cxx_tmpl_param_visitor, &tpvd );
  if ( !tpvd.found_tmpl_type_param )
    return true;                        // pack, out of range, etc.: can't tell

  bool              is_param_constructed = true;
  CXString const    name_cxs = clang_getCursorSpelling( tpvd.found_csr );
  char const *const name = clang_getCString( name_cxs );

  if ( name != NULL && name[0] != '\0' ) {
    cxx_tmpl_ctors_visitor_data tcvd = { .find_param_name = name };
    tidy_visitClassChildren( tmpl_csr, &cxx_tmpl_ctors_visitor, &tcvd );
    is_param_constructed = tcvd.is_param_constructed;
  }

  clang_disposeString( name_cxs );
  return is_param_constructed;
}

/**
 * A helper function for cxx_tmpl_ctors_param() that visits everything inside a
 * class template looking for evidence that it constructs its type parameter.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The cxx_tmpl_ctors_visitor_data to use.
 * @return Returns `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
static enum CXChildVisitResult cxx_tmpl_ctors_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;

  enum CXCursorKind kind = clang_getCursorKind( cursor );
  CXType            type = clang_getCursorType( cursor );

  switch ( kind ) {
    case CXCursor_CallExpr:;            // T{} (dependent -> unresolved)
      CXCursor const ref_csr = clang_getCursorReferenced( cursor );
      if ( !tidy_Cursor_isInvalid( ref_csr ) )
        goto skip;                      // a resolved call, not a construction
      break;

    case CXCursor_CXXBaseSpecifier:     // struct S : T { };
    case CXCursor_FieldDecl:            // struct S { T f; };
    case CXCursor_VarDecl:              // T x; T a[N]; std::vector<T> v;
      break;

    case CXCursor_CXXNewExpr:           // new T; new T[n]
      type = clang_getPointeeType( type );
      break;

    default:
      goto skip;
  } // switch

  assert( data != NULL );
  cxx_tmpl_ctors_visitor_data *const tcvd = data;
  if ( cxx_type_ctors_param( type, tcvd->find_param_name, /*depth=*/0 ) ) {
    tcvd->is_param_constructed = true;
    return CXChildVisit_Break;
  }

skip:
  return CXChildVisit_Recurse;
}

/**
 * A helper function for cxx_tmpl_ctors_param() that visits a template's direct
 * children looking for its i-th template parameter.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The cxx_tmpl_param_visitor_data to use.
 * @return Returns `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult cxx_tmpl_param_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;

  enum CXCursorKind kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_TemplateTemplateParameter:
    case CXCursor_TemplateTypeParameter:;
      cxx_tmpl_param_visitor_data *const tpvd = data;
      if ( tpvd->curr_idx == tpvd->find_idx ) {
        if ( kind == CXCursor_TemplateTypeParameter ) {
          tpvd->found_csr = cursor;
          tpvd->found_tmpl_type_param = true;
        }
        return CXChildVisit_Break;
      }
      ++tpvd->curr_idx;
      break;
    default:
      /* suppress warning */;
  } // switch

  return CXChildVisit_Continue;
}

/**
 * Gets whether a variable, array, field, or base class of \a type would cause
 * an object of the parameterized type named \a param_name to be constructed.
 *
 * @remarks For a template that has parameterized types (e.g., `std::set<T>`,
 * where `T` is a template parameter as opposed to `std::set<MyClass>` where
 * `MyClass` is a concrete type), it's assumed that the template might
 * construct objects of the types, so its parameterized types are checked
 * recursively.
 *
 * @param type The type to check.
 * @param find_param_name The name of the parameterized type to find, e.g.,
 * `T`.
 * @param depth The current recursion depth.  The initial call should pass 0.
 * Recursion stops (returning `false`) past a fixed limit to guard against
 * pathological types.
 * @return Returns `true` only if \a type constructs an object of the
 * parameterized type named \a find_param_name.
 *
 * @sa cxx_tmpl_constructs_param()
 */
NODISCARD
static bool cxx_type_ctors_param( CXType type, char const *find_param_name,
                                  unsigned depth ) {
  assert( find_param_name != NULL );

  if ( unlikely( depth > 16 ) )
    return false;                       // guard against runaway recursion

  CXCursor decl_csr;

  for (;;) {
    switch ( type.kind ) {
      case CXType_ConstantArray:
      case CXType_DependentSizedArray:
      case CXType_IncompleteArray:
      case CXType_VariableArray:
        type = clang_getArrayElementType( type );
        continue;

      case CXType_Elaborated:
        type = clang_Type_getNamedType( type );
        continue;

      case CXType_FunctionNoProto:
      case CXType_FunctionProto:
      case CXType_Invalid:
      case CXType_LValueReference:
      case CXType_MemberPointer:
      case CXType_Pointer:
      case CXType_RValueReference:
        return false;

      case CXType_Typedef:
        decl_csr = clang_getTypeDeclaration( type );
        type = clang_getTypedefDeclUnderlyingType( decl_csr );
        continue;

      default:
        /* suppress warning */;
    } // switch
    break;
  } // for

  decl_csr = clang_getTypeDeclaration( type );
  enum CXCursorKind kind = clang_getCursorKind( decl_csr );

  if ( kind == CXCursor_TemplateTypeParameter ) {
    CXString const    name_cxs = clang_getCursorSpelling( decl_csr );
    char const *const name = clang_getCString( name_cxs );
    bool const found = name != NULL && strcmp( name, find_param_name ) == 0;
    clang_disposeString( name_cxs );
    return found;
  }

  int const num_args = clang_Type_getNumTemplateArguments( type );
  for ( int i = 0; i < num_args; ++i ) {
    CXType const arg_type =
      clang_Type_getTemplateArgumentAsType( type, STATIC_CAST( unsigned, i ) );
    if ( cxx_type_ctors_param( arg_type, find_param_name, depth + 1 ) )
      return true;
  } // for

  return false;
}

/**
 * Adds a reference to the destructor of every class-type template argument of
 * \a type, recursively.
 *
 * @remarks Such a destructor is called from within the template (e.g.,
 * `std::default_delete<T>::operator()`), but libclang's C API doesn't expose
 * template instantiation bodies, so we have to infer the call.
 *
 * @param type The type to check.
 * @param svd The symbols_visit_data to use.
 */
static void cxx_type_template_args_dtor_add_ref( CXType type,
                                                 symbols_visit_data *svd ) {
  int num_args = clang_Type_getNumTemplateArguments( type );
  if ( num_args <= 0 ) {
    type = clang_getCanonicalType( type );
    num_args = clang_Type_getNumTemplateArguments( type );
  }

  for ( int i = 0; i < num_args; ++i ) {
    CXType arg_type =
      clang_Type_getTemplateArgumentAsType( type, STATIC_CAST( unsigned, i ) );
    if ( arg_type.kind == CXType_Invalid )
      continue;                         // non-type argument

    arg_type = tidy_Type_getUnderlyingType( arg_type );
    CXCursor const arg_cls_csr = clang_getTypeDeclaration( arg_type );
    if ( tidy_Cursor_isClassDecl( arg_cls_csr ) )
      cxx_dtor_add_ref( arg_cls_csr, svd );

    cxx_type_template_args_dtor_add_ref( arg_type, svd );
  } // for
}

/**
 * Prints statistics for C++ ADL hash tables if requested.
 */
static void print_statistics( void ) {
  if ( !verbose_print_statistics() )
    return;

  verbose_printf(
    "  adl-name set:\n"
  );
  verbose_printf(
    "    adl-ns-load-factor = " TIDY_STAT_LF_FMT "\n",
                              ht_table_load_factor( &cxx_adl_name_set )
  );
  verbose_printf(
    "    adl-ns-size = %u\n", cxx_adl_name_set.size
  );

  verbose_printf(
    "  adl-type set:\n"
  );
  verbose_printf(
    "    adl-ts-load-factor = " TIDY_STAT_LF_FMT "\n",
                              ht_table_load_factor( &cxx_adl_type_set )
  );
  verbose_printf(
    "    adl-ts-size = %u\n", cxx_adl_type_set.size
  );
}

/**
 * Inserts a string to a lazily initialized string set.
 *
 * @param set The set to add to.  If its buckets are NULL, it's initialized
 * first.
 * @param s The string to add.  It is copied.
 */
static void str_set_insert( hash_table_t *set, char const *s ) {
  assert( set != NULL );
  assert( s != NULL );

  if ( set->buckets == NULL ) {
    ht_table_init(
      set, HT_DINT, 2.0, 256,
      POINTER_CAST( ht_cmp_fn_t, &strcmp ),
      POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
    );
  }
  ht_table_insert( set, CONST_CAST( char*, s ), strlen( s ) + 1/*'\0'*/ );
}

////////// extern functions ///////////////////////////////////////////////////

void cxx_adl_collect_classes( CXType type, bool skip_top, hash_table_t *set ) {
  assert( set != NULL );

  type = clang_getCanonicalType( type );
  for (;;) {
    CXType inner = clang_getPointeeType( type );
    if ( inner.kind == CXType_Invalid )
      inner = clang_getArrayElementType( type );
    if ( inner.kind == CXType_Invalid )
      break;
    type = clang_getCanonicalType( inner );
  } // for

  if ( type.kind == CXType_Invalid )
    return;

  if ( !skip_top ) {
    CXCursor decl_csr = clang_getTypeDeclaration( type );
    enum CXCursorKind kind = clang_getCursorKind( decl_csr );
    if ( kind == CXCursor_EnumDecl || tidy_Cursor_isClassDecl( decl_csr ) ) {
      //
      // Use the primary template, if any, so that vec<int> and vec<T> have the
      // same key.
      //
      CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( decl_csr );
      if ( !clang_Cursor_isNull( tmpl_csr ) )
        decl_csr = tmpl_csr;

      if ( !tidy_Cursor_isInSystemHeader( decl_csr ) ) {
        char *const usr = tidy_getCursorUSR( decl_csr );
        if ( usr != NULL ) {
          str_set_insert( set, usr );
          free( usr );
        }
      }
    }
  }

  int const num_args = clang_Type_getNumTemplateArguments( type );
  for ( int i = 0; i < num_args; ++i ) {
    CXType const arg_type =
      clang_Type_getTemplateArgumentAsType( type, (unsigned)i );
    cxx_adl_collect_classes( arg_type, /*skip_top=*/false, set );
  } // for
}

void cxx_adl_name_set_add( char const *name ) {
  str_set_insert( &cxx_adl_name_set, name );
}

void cxx_adl_type_set_add( CXType type ) {
  type = clang_getCanonicalType( type );
  if ( type.kind == CXType_FunctionProto ) {
    CXType const res_type = clang_getResultType( type );
    cxx_adl_collect_classes( res_type, /*skip_top=*/true, &cxx_adl_type_set );
    int const num_args = clang_getNumArgTypes( type );
    for ( int i = 0; i < num_args; ++i ) {
      CXType const arg_type = clang_getArgType( type, (unsigned)i );
      cxx_adl_collect_classes( arg_type, /*skip_top=*/true, &cxx_adl_type_set );
    } // for
  }
  else {
    cxx_adl_collect_classes( type, /*skip_top=*/true, &cxx_adl_type_set );
  }
}

enum CXChildVisitResult cxx_def_ctor_add_ref_visitor( CXCursor cursor,
                                                      CXCursor parent,
                                                      CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );

  if ( kind == CXCursor_FieldDecl ) {
    cxx_field_def_ctor_add_ref( cursor, svd );
    return CXChildVisit_Continue;
  }

  if ( kind != CXCursor_CXXBaseSpecifier )
    goto skip;

  CXCursor base_cls_csr = tidy_Cursor_getBaseSpecifierClass( cursor );
  if ( tidy_Cursor_isInvalid( base_cls_csr ) ) {
    CXType const type = clang_getCursorType( cursor );
    base_cls_csr = clang_getTypeDeclaration( type );
  }
  base_cls_csr = clang_getCanonicalCursor( base_cls_csr );

  if ( tidy_Cursor_callsBaseConstructor( svd->cxx_constructor.ctor_csr,
                                         base_cls_csr ) ) {
    goto skip;
  }

  //
  // If no constructor of base_cls_csr was explicitly called, add a reference
  // to the default constructor, if any.
  //
  CXCursor const def_ctor = tidy_Cursor_getDefaultConstructor( base_cls_csr );
  if ( !tidy_Cursor_isInvalid( def_ctor ) )
    cursor_add_ref( def_ctor, svd );

skip:
  //
  // We can't return CXChildVisit_Break even if we find either an explicit base
  // class constructor call or an implicit base class default constructor call
  // since there may be multiple base classes.  Hence, we always have to return
  // CXChildVisit_Continue.
  //
  return CXChildVisit_Continue;
}

enum CXChildVisitResult cxx_DeleteExpr_visitor( CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData data ) {
  (void)parent;

  CXType type = clang_getCursorType( cursor );
  if ( type.kind == CXType_Invalid )
    goto skip;
  type = clang_getCanonicalType( type );
  type = clang_getNonReferenceType( type );
  if ( type.kind != CXType_Pointer )
    goto skip;

  CXType pointee_type = clang_getPointeeType( type );
         pointee_type = clang_getCanonicalType( pointee_type );
         pointee_type = clang_getNonReferenceType( pointee_type );

  CXCursor const cls_csr = clang_getTypeDeclaration( pointee_type );

  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    symbols_visit_data *const svd = data;
    cxx_dtor_add_ref( cls_csr, svd );
    cxx_operator_add_refs(
      cls_csr, "operator delete", "operator delete[]", svd
    );
    return CXChildVisit_Break;
  }

skip:
  return CXChildVisit_Recurse;
}

void cxx_dtors_add_refs( CXCursor decl_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  CXCursor const cls_csr = tidy_Cursor_getUnderlyingTypeDecl( decl_csr );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  cxx_dtor_add_ref( cls_csr, svd );

  CXType  decl_type = clang_getCursorType( decl_csr );
          decl_type = tidy_Type_getUnderlyingType( decl_type );
  cxx_type_template_args_dtor_add_ref( decl_type, svd );
}

void cxx_dtor_member_add_refs( CXCursor dtor_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  CXCursor const cls_csr = clang_getCursorSemanticParent( dtor_csr );
  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    tidy_visitClassChildren( cls_csr, &cxx_field_dtor_add_ref_visitor, svd );
    tidy_visitClassChildren( cls_csr, &cxx_base_dtor_add_ref_visitor, svd );
  }
}

void cxx_field_def_ctor_add_ref( CXCursor field_csr, symbols_visit_data *svd ) {
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  CXCursor const init_csr = tidy_Cursor_getFieldInitializer( field_csr );

  if ( !clang_Cursor_isNull( init_csr ) ) {
    enum CXCursorKind const kind = clang_getCursorKind( init_csr );
    if ( kind == CXCursor_Constructor ) {
      //
      // The field is explicitly initialized via a constructor's mem-
      // initializer.
      //
      tidy_symbol *const ctor_sym = symbol_add( init_csr, svd->source );
      if ( ctor_sym != NULL )
        symbol_add_ref( svd->curr_sym, ctor_sym );
    }
    //
    // Otherwise the field is explicitly initialized via an in-class
    // assignment.
    //
    return;
  }

  //
  // The field isn't explicitly initialized: see if it's of a type that has a
  // default constructor.
  //
  CXCursor const cls_csr = tidy_Cursor_getUnderlyingTypeDecl( field_csr );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  tidy_ConstructorInfo ctor_info;
  if ( tidy_Cursor_getConstructorInfo( cls_csr, &ctor_info ) &&
       !clang_Cursor_isNull( ctor_info.def_ctor_csr ) ) {
    //
    // The field is implicitly initialized via a default constructor.
    //
    cursor_add_ref( ctor_info.def_ctor_csr, svd );
  }
  else {
    //
    // The field is not initialized via a default constructor, but the class's
    // fields might be.
    //
    tidy_visitClassChildren(
      cls_csr, &cxx_field_def_ctor_add_ref_visitor, svd
    );
  }
}

enum
CXChildVisitResult cxx_field_def_ctor_add_ref_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind == CXCursor_FieldDecl ) {
    symbols_visit_data *const svd = data;
    cxx_field_def_ctor_add_ref( cursor, svd );
  }
  return CXChildVisit_Continue;
}

enum CXChildVisitResult cxx_find_begin_end_visitor( CXCursor cursor,
                                                    CXCursor parent,
                                                    CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  int const num_args = clang_Cursor_getNumArguments( cursor );

  switch ( kind ) {
    case CXCursor_CXXMethod:            // func() [const]
      if ( num_args != 0 )
        goto skip;
      bool const is_method_const = clang_CXXMethod_isConst( cursor );
      if ( is_method_const != svd->cxx_for_range.is_const )
        goto skip;

      break;

    case CXCursor_FunctionDecl:         // func( T [const]& )
      if ( num_args != 1 )
        goto skip;
      CXCursor const arg_csr = clang_Cursor_getArgument( cursor, 0 );
      CXType const param_type = clang_getCursorType( arg_csr );
      switch ( param_type.kind ) {
        case CXType_LValueReference:
        case CXType_RValueReference:
          break;
        default:
          goto skip;
      } // switch

      CXType const non_ref_type = clang_getNonReferenceType( param_type );
      CXType const canon_type = clang_getCanonicalType( non_ref_type );

      bool const is_param_const =
        clang_isConstQualifiedType( non_ref_type ) ||
        clang_isConstQualifiedType( canon_type );
      if ( is_param_const != svd->cxx_for_range.is_const )
        goto skip;

      CXCursor param_cls_csr = clang_getTypeDeclaration( canon_type );
      if ( tidy_Cursor_isInvalid( param_cls_csr ) )
        goto skip;
      param_cls_csr = clang_getCanonicalCursor( param_cls_csr );
      if ( !clang_equalCursors( param_cls_csr, svd->cxx_for_range.cls_csr ) )
        goto skip;
      break;

    default:
      goto skip;
  } // switch

  int const match = tidy_Cursor_isSpellingEqualToAny( cursor, "begin", "end" );
  switch ( match ) {
    case 0:
      if ( clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) )
        svd->cxx_for_range.begin_csr = cursor;
      break;
    case 1:
      if ( clang_Cursor_isNull( svd->cxx_for_range.end_csr ) )
        svd->cxx_for_range.end_csr = cursor;
      break;
  } // switch

  if ( !clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) &&
       !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
    return CXChildVisit_Break;
  }

skip:
  return CXChildVisit_Continue;
}

void cxx_member_add_ref( CXCursor member_csr, symbols_visit_data *svd ) {
  CXCursor const base_csr = tidy_Cursor_getFirstExprChild( member_csr );
  if ( clang_Cursor_isNull( base_csr ) )
    return;

  CXType type = clang_getCanonicalType( clang_getCursorType( base_csr ) );
         type = clang_getNonReferenceType( type );
  if ( type.kind == CXType_Pointer ) {
    type = clang_getPointeeType( type );
    type = clang_getCanonicalType( type );
  }

  CXCursor const cls_csr = clang_getTypeDeclaration( type );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;                             // e.g., bare template parameter

  //
  // clang_getCursorSpelling() returns "" for a dependent member, so get the
  // member's name from the tokens: it's the first identifier after the last
  // "." or "->".  If there's neither (an implicit "this->"), it's the first
  // identifier.
  //
  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( member_csr );
  CXSourceRange const     range = tidy_getCursorExtent( member_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tu, range, &tokens, &token_count );
  if ( token_count == 0 )
    return;                             // LCOV_EXCL_LINE

  unsigned start_idx = 0;
  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Punctuation )
      continue;
    if ( tidy_Token_isSpellingEqualToAny( tu, tokens[i], ".", "->" ) >= 0 )
      start_idx = i + 1;
  } // for

  for ( unsigned i = start_idx; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;
    CXString const    name_cxs = clang_getTokenSpelling( tu, tokens[i] );
    char const *const name = clang_getCString( name_cxs );
    cxx_member_add_refs_by_name( cls_csr, name, svd );
    clang_disposeString( name_cxs );
    break;
  } // for

  clang_disposeTokens( tu, tokens, token_count );
}

void cxx_member_def_ctor_add_ref( CXCursor member_csr, tidy_symbol *member_sym,
                                  symbols_visit_data const *svd ) {
  assert( member_sym != NULL );
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  enum CXCursorKind const kind = clang_getCursorKind( member_csr );
  switch ( kind ) {
    case CXCursor_CXXMethod:
      if ( clang_CXXMethod_isStatic( member_csr ) )
        return;
      break;
    case CXCursor_FieldDecl:
      break;
    default:
      return;
  } // switch

  CXCursor const        cls_csr = clang_getCursorSemanticParent( member_csr );
  tidy_ConstructorInfo  ctor_info;

  if ( !tidy_Cursor_getConstructorInfo( cls_csr, &ctor_info ) )
    return;
  if ( ctor_info.has_other_ctors )
    return;
  if ( clang_Cursor_isNull( ctor_info.def_ctor_csr ) )
    return;

  tidy_symbol *const def_ctor_sym =
    symbol_add( ctor_info.def_ctor_csr, svd->source );
  if ( def_ctor_sym != NULL )
    symbol_add_ref( member_sym, def_ctor_sym );
}

bool cxx_operator_add_refs( CXCursor cls_csr, char const *name,
                            char const *array_name, symbols_visit_data *svd ) {
  assert( svd != NULL );

  svd_cxx_operator const prev_svd_cxx_operator = svd->cxx_operator;
  svd->cxx_operator = (svd_cxx_operator){
    .name = name,
    .array_name = array_name
  };

  tidy_visitClassChildren( cls_csr, &cxx_operator_add_refs_visitor, svd );
  if ( !svd->cxx_operator.found )
    tidy_visitClassChildren( cls_csr, &cxx_operator_base_visitor, svd );

  bool const found = svd->cxx_operator.found;
  svd->cxx_operator = prev_svd_cxx_operator;
  return found;
}

void cxx_var_template_args_def_ctor_add_ref( CXType type,
                                             symbols_visit_data *svd ) {
  int num_args = clang_Type_getNumTemplateArguments( type );
  if ( num_args <= 0 ) {
    type = clang_getCanonicalType( type );
    num_args = clang_Type_getNumTemplateArguments( type );
  }

  CXType const canon_type = clang_getCanonicalType( type );
  CXCursor const canon_csr = clang_getTypeDeclaration( canon_type );

  for ( int i = 0; i < num_args; ++i ) {
    CXType arg_type =
      clang_Type_getTemplateArgumentAsType( type, STATIC_CAST( unsigned, i ) );
    if ( arg_type.kind == CXType_Invalid )
      continue;

    arg_type = clang_getCanonicalType( arg_type );
    CXCursor const arg_cls_csr = clang_getTypeDeclaration( arg_type );

    if ( tidy_Cursor_isClassDecl( arg_cls_csr ) &&
         cxx_tmpl_ctors_param( canon_csr, STATIC_CAST( unsigned, i ) ) ) {
      tidy_ConstructorInfo ctor_info;
      if ( tidy_Cursor_getConstructorInfo( arg_cls_csr, &ctor_info ) &&
           !clang_Cursor_isNull( ctor_info.def_ctor_csr ) ) {
        cursor_add_ref( ctor_info.def_ctor_csr, svd );
      }
    }

    //
    // Visit nested template arguments (e.g., std::vector<std::vector<T>>)
    // recursively.
    //
    cxx_var_template_args_def_ctor_add_ref( arg_type, svd );
  } // for
}

void cxx_symbols_init( void ) {
  ASSERT_RUN_ONCE();
  ATEXIT( &cxx_symbols_cleanup );
}

void cxx_symbols_mark_adl_roots( void ) {
  if ( cxx_adl_name_set.size == 0 || cxx_adl_type_set.size == 0 )
    return;

  ht_iterator_t sym_iter;
  ht_iterator_init( &sym_iter, &tidy_symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &sym_iter )) != NULL; ) {
    if ( sym->is_root || sym->cxx_adl_usr_set.size == 0 )
      continue;

    char const *const unqual_name = cxx_unqual_name( sym->name );
    if ( ht_table_find( &cxx_adl_name_set, unqual_name ) == NULL )
      continue;

    ht_iterator_t usr_iter;
    ht_iterator_init( &usr_iter, &sym->cxx_adl_usr_set );
    for ( char const *usr; (usr = ht_iterator_next( &usr_iter )) != NULL; ) {
      if ( ht_table_find( &cxx_adl_type_set, usr ) != NULL ) {
        sym->is_root = true;
        break;
      }
    } // for
  } // for
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
