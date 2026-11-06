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
#include "clang_util.h"
#include "source.h"
#include "symbol.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

/// @endcond

/**
 * @addtogroup tidy-cxx-symbol-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct cxx_dtor_frame cxx_dtor_frame;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A class whose implicit destructor is being expanded; used to break cycles.
 */
struct cxx_dtor_frame {
  CXCursor              cls_csr;        ///< TODO.
  cxx_dtor_frame const *prev;           ///< TODO.
};

////////// local functions ////////////////////////////////////////////////////

static void cxx_dtor_add_ref( CXCursor, symbols_visit_data* );

static enum CXChildVisitResult
            cxx_field_dtor_add_ref_visitor( CXCursor, CXCursor, CXClientData ),
            cxx_member_add_refs_by_name_visitor( CXCursor, CXCursor,
                                                 CXClientData ),
            cxx_member_by_name_base_visitor( CXCursor, CXCursor, CXClientData );

static void cxx_type_template_args_dtor_add_ref( CXType, symbols_visit_data* );

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

  CXCursor base_cls_csr = clang_getCursorReferenced( cursor );
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
    cursor_add_ref( dtor_csr, svd );
    return;
  }

  cls_csr = clang_getCanonicalCursor( cls_csr );
  for ( cxx_dtor_frame const *frame = svd->cxx_dtor.top; frame != NULL;
        frame = frame->prev ) {
    if ( clang_equalCursors( frame->cls_csr, cls_csr ) )
      return;
  } // for
  cxx_dtor_frame const frame = {
    .cls_csr = cls_csr,
    .prev = svd->cxx_dtor.top
  };
  svd->cxx_dtor.top = &frame;

  // Implicit destructor calls member and base destructors.
  clang_visitChildren( cls_csr, &cxx_field_dtor_add_ref_visitor, svd );
  clang_visitChildren( cls_csr, &cxx_base_dtor_add_ref_visitor, svd );

  //
  // An implicit instantiation exposes no children, so also visit the template
  // it was instantiated from.  Fields and bases whose types are concrete
  // (e.g., Foo f;) resolve there; those that depend on a template parameter
  // (e.g., T f;) are skipped since they aren't class declarations.
  //
  CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( cls_csr );
  if ( !tidy_Cursor_isInvalid( tmpl_csr ) ) {
    clang_visitChildren( tmpl_csr, &cxx_field_dtor_add_ref_visitor, svd );
    clang_visitChildren( tmpl_csr, &cxx_base_dtor_add_ref_visitor, svd );
  }

  svd->cxx_dtor.top = frame.prev;
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

  clang_visitChildren( cls_csr, &cxx_member_add_refs_by_name_visitor, svd );

  //
  // If cls_csr is an implicit instantiation, libclang doesn't expose its
  // members, so look in the template it was instantiated from.
  //
  if ( !svd->cxx_member.found ) {
    CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( cls_csr );
    if ( !tidy_Cursor_isInvalid( tmpl_csr ) ) {
      clang_visitChildren(
        tmpl_csr, &cxx_member_add_refs_by_name_visitor, svd
      );
    }
  }

  //
  // Per C++ name hiding, a member found in the class hides the same name in
  // its bases, so look in the bases only if nothing was found.
  //
  if ( !svd->cxx_member.found )
    clang_visitChildren( cls_csr, &cxx_member_by_name_base_visitor, svd );

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

  CXCursor const base_csr =
    clang_getTypeDeclaration( clang_getCursorType( cursor ) );
  if ( !tidy_Cursor_isClassDecl( base_csr ) )
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

  CXCursor const base_csr =
    clang_getTypeDeclaration( clang_getCursorType( cursor ) );
  if ( !tidy_Cursor_isClassDecl( base_csr ) )
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
    if ( num_args <= 0 )
      return;
  }

  for ( unsigned i = 0; i < STATIC_CAST( unsigned, num_args ); ++i ) {
    CXType arg_type = clang_Type_getTemplateArgumentAsType( type, i );
    if ( arg_type.kind == CXType_Invalid )
      continue;                         // non-type argument

    arg_type = tidy_Type_getUnderlyingType( arg_type );
    CXCursor const arg_cls_csr = clang_getTypeDeclaration( arg_type );
    if ( tidy_Cursor_isClassDecl( arg_cls_csr ) )
      cxx_dtor_add_ref( arg_cls_csr, svd );

    cxx_type_template_args_dtor_add_ref( arg_type, svd );
  } // for
}

////////// extern functions ///////////////////////////////////////////////////

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

  CXCursor base_cls_csr = clang_getCursorReferenced( cursor );
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
    clang_visitChildren( cls_csr, &cxx_field_dtor_add_ref_visitor, svd );
    clang_visitChildren( cls_csr, &cxx_base_dtor_add_ref_visitor, svd );
  }
}

void cxx_field_def_ctor_add_ref( CXCursor field_csr, symbols_visit_data *svd ) {
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  CXCursor const init_csr = tidy_Cursor_getFieldInitializer( field_csr );

  if ( !clang_Cursor_isNull( init_csr ) ) {
    enum CXCursorKind const kind = clang_getCursorKind( init_csr );
    if ( kind == CXCursor_Constructor ) {
      //
      // The field is initialized explicitly via a constructor's mem-
      // initializer.
      //
      tidy_symbol *const ctor_sym = symbol_add( init_csr, svd->source );
      if ( ctor_sym != NULL )
        symbol_add_ref( svd->curr_sym, ctor_sym );
    }
    //
    // Otherwise the field is initialized explicitly via an in-class
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
    // The field is initialized implicitly via a default constructor.
    //
    cursor_add_ref( ctor_info.def_ctor_csr, svd );
  }
  else {
    //
    // The field is not initialized via a default constructor, but the class's
    // fields might be.
    //
    clang_visitChildren( cls_csr, &cxx_field_def_ctor_add_ref_visitor, svd );
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

  clang_visitChildren( cls_csr, &cxx_operator_add_refs_visitor, svd );
  if ( !svd->cxx_operator.found )
    clang_visitChildren( cls_csr, &cxx_operator_base_visitor, svd );

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
    if ( num_args <= 0 )
      return;
  }

  for ( unsigned i = 0; i < STATIC_CAST( unsigned, num_args ); ++i ) {
    CXType const arg_type = clang_Type_getTemplateArgumentAsType( type, i );
    if ( arg_type.kind == CXType_Invalid )
      continue;

    CXType const    canon_arg_type = clang_getCanonicalType( arg_type );
    CXCursor const  arg_cls_csr = clang_getTypeDeclaration( canon_arg_type );

    if ( tidy_Cursor_isClassDecl( arg_cls_csr ) ) {
      tidy_ConstructorInfo ctor_info;
      if ( tidy_Cursor_getConstructorInfo( arg_cls_csr, &ctor_info ) &&
           !clang_Cursor_isNull( ctor_info.def_ctor_csr ) &&
           !ctor_info.has_other_ctors ) {
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

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
