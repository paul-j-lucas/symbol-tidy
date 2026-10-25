/*
**      symbol-tidy -- Symbol tidier
**      src/clang_util.c
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
 * Defines utility functions for libclang.
 */

// local
#include "pjl_config.h"
#include "clang_util.h"
#include "str_util.h"
#include "strbuf.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup clang-util-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct callsBaseConstructor_data  callsBaseConstructor_data;
typedef struct getCursorByName_data       getCursorByName_data;
typedef CXString                        (*getCursorName_fn)( CXCursor );

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data passed to callsBaseConstructor_visitor().
 */
struct callsBaseConstructor_data {
  CXCursor  base_cls_csr;               ///< The base class cursor.
  bool      found_explicit_call;        ///< Called base constructor explicitly?
};

/**
 * Additional data passed to getCursorByName_visitor().
 */
struct getCursorByName_data {
  char const *find_name;                ///< The name to find.
  CXCursor    found_csr;                ///< The name's cursor, if found.
  CXCursor    skip_csr;                 ///< Skip this cursor.
  bool        cxx_recurse_into_scope;   ///< C++: recurse into scope?
};

////////// local functions ////////////////////////////////////////////////////

/**
 * A helper function for tidy_Cursor_callsBaseConstructor() that looks for an
 * explicit base class constructor call.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The getCursorByName_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
static
enum CXChildVisitResult callsBaseConstructor_visitor( CXCursor cursor,
                                                      CXCursor parent,
                                                      CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_CallExpr )
    goto skip;

  CXCursor const ref_csr = clang_getCursorReferenced( cursor );
  if ( tidy_Cursor_isInvalid( ref_csr ) )
    goto skip;

  enum CXCursorKind const ref_kind = clang_getCursorKind( ref_csr );
  if ( ref_kind != CXCursor_Constructor )
    goto skip;

  CXCursor sem_parent = clang_getCursorSemanticParent( ref_csr );
  sem_parent = clang_getCanonicalCursor( sem_parent );

  assert( data != NULL );
  callsBaseConstructor_data *const cbcd = data;
  if ( clang_equalCursors( sem_parent, cbcd->base_cls_csr ) ) {
    cbcd->found_explicit_call = true;
    return CXChildVisit_Break;
  }

skip:
  return CXChildVisit_Continue;
}

/**
 * A helper function for tidy_Cursor_getConstructorInfo().
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data A pointer to receive that the class has a constuctor.
 * @return Returns `CXChildVisit_Break` only if a base class is found;
 * otherwise returns `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult getConstructorInfo_visitor( CXCursor cursor,
                                                           CXCursor parent,
                                                           CXClientData data ) {
  (void)parent;
  assert( data != NULL );
  tidy_ConstructorInfo *const ctor_info = data;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_CXXBaseSpecifier:;
      CXCursor base_cls_csr = clang_getCursorReferenced( cursor );
      if ( tidy_Cursor_isInvalid( base_cls_csr ) ) {
        CXType const type = clang_getCursorType( cursor );
        base_cls_csr = clang_getTypeDeclaration( type );
      }
      if ( tidy_Cursor_isInvalid( base_cls_csr ) )
        break;
      if ( !tidy_Cursor_getConstructorInfo( base_cls_csr, ctor_info ) )
        break;
      FALLTHROUGH;

    case CXCursor_Constructor:
      if ( clang_CXXConstructor_isDefaultConstructor( cursor ) ) {
        if ( clang_Cursor_isNull( ctor_info->default_ctor_csr ) )
          ctor_info->default_ctor_csr = cursor;
      }
      else if ( !clang_CXXConstructor_isCopyConstructor( cursor ) &&
                !clang_CXXConstructor_isMoveConstructor( cursor ) ) {
        ctor_info->has_other_ctors = true;
        return CXChildVisit_Break;
      }
      break;

    case CXCursor_UsingDeclaration:;
      CXCursor const ref_csr = clang_getCursorReferenced( cursor );
      if ( tidy_Cursor_isInvalid( ref_csr ) )
        break;
      enum CXCursorKind const ref_kind = clang_getCursorKind( ref_csr );
      if ( ref_kind != CXCursor_Constructor )
        break;
      if ( !clang_CXXConstructor_isDefaultConstructor( ref_csr ) &&
           !clang_CXXConstructor_isCopyConstructor( ref_csr ) &&
           !clang_CXXConstructor_isMoveConstructor( ref_csr ) ) {
        ctor_info->has_other_ctors = true;
        return CXChildVisit_Break;
      }
      break;

    default:
      /* suppress warning */;
  } // switch

  return CXChildVisit_Continue;
}

/**
 * A helper function for tidy_getCursorByName() that visits each symbol within
 * a scope attempting to find one having \ref getCursorByName_data::find_name
 * "find_name".
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The getCursorByName_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
static enum CXChildVisitResult getCursorByName_visitor( CXCursor cursor,
                                                        CXCursor parent,
                                                        CXClientData data ) {
  (void)parent;
  assert( data != NULL );

  getCursorByName_data *const gcbnd = data;

  if ( clang_equalCursors( cursor, gcbnd->skip_csr ) )
    goto skip;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_CXXBaseSpecifier:;
      CXCursor base_cls_csr = clang_getCursorReferenced( cursor );
      if ( tidy_Cursor_isInvalid( base_cls_csr ) ) {
        CXType const type = clang_getCursorType( cursor );
        base_cls_csr = clang_getTypeDeclaration( type );
      }
      if ( !tidy_Cursor_isInvalid( base_cls_csr ) )
        clang_visitChildren( base_cls_csr, &getCursorByName_visitor, data );
      break;

    default:
      if ( !clang_isDeclaration( kind ) )
        break;
      FALLTHROUGH;

    case CXCursor_MacroDefinition:
      if ( tidy_Cursor_isSpellingEqualTo( cursor, gcbnd->find_name) ) {
        gcbnd->found_csr = cursor;
        return CXChildVisit_Break;
      }
      break;

    case CXCursor_Namespace:
      if ( gcbnd->cxx_recurse_into_scope )
        return CXChildVisit_Recurse;
      break;
        
  } // switch

skip:
  return CXChildVisit_Continue;
}

/**
 * A helper function for tidy_Cursor_getDefaultConstructor() that visits each
 * symbol attempting to find a default constructor.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data A pointer to receive the cursor for the default constructor.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
static
enum CXChildVisitResult getDefaultConstructor_visitor( CXCursor cursor,
                                                       CXCursor parent,
                                                       CXClientData data ) {
  (void)parent;

  if ( clang_CXXConstructor_isDefaultConstructor( cursor ) ) {
    assert( data != NULL );
    CXCursor *const default_ctor_csr = data;
    *default_ctor_csr = cursor;
    return CXChildVisit_Break;
  }

  return CXChildVisit_Continue;
}

/**
 * A helper function for tidy_Cursor_getDestructor() that visits each symbol
 * attempting to find a destructor.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data A pointer to receive the cursor for the default constructor.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
static enum CXChildVisitResult getDestructor_visitor( CXCursor cursor,
                                                      CXCursor parent,
                                                      CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind == CXCursor_Destructor ) {
    assert( data != NULL );
    CXCursor *const dtor_csr = data;
    *dtor_csr = cursor;
    return CXChildVisit_Break;
  }

  return CXChildVisit_Continue;
}

/**
 * Given a cursor at a local name of an enumeration, class, class data member,
 * class member function, structure, union, or namespace, gets the fully scoped
 * name skipping inline namespaces.
 *
 * @param cursor The cursor at a symbol.
 * @param name_fn The libclang function to use to get the name of \a cursor.
 * @param sbuf The strbuf to use.
 */
static void getScopedName_impl( CXCursor cursor, getCursorName_fn name_fn,
                                strbuf_t *sbuf ) {
  assert( name_fn != NULL );
  assert( sbuf != NULL );

  CXCursor parent = cursor;
  do {
    parent = clang_getCursorSemanticParent( parent );
  } while ( clang_Cursor_isInlineNamespace( parent ) );

  if ( tidy_Cursor_isScopeDecl( parent ) )
    getScopedName_impl( parent, name_fn, sbuf );

  CXString const name_cxs = (*name_fn)( cursor );
  char const *const name = null_if_empty( clang_getCString( name_cxs ) );
  if ( name == NULL || name[0] == '(' )
    goto done;  // Skip "(anonymous ...)" and "(unnamed ...)".

  if ( sbuf->len > 0 )
    strbuf_putsn( sbuf, "::", STRLITLEN( "::" ) );
  strbuf_puts( sbuf, name );

done:
  clang_disposeString( name_cxs );
}

/**
 * Helper function for both tidy_Cursor_getScopedDisplayName() and
 * tidy_Cursor_getScopedSpelling().
 *
 * @param cursor The cursor at a symbol.
 * @param name_fn The libclang function to use to get the name of \a cursor.
 * @return Returns the fully scoped name.  The caller is responsible for
 * freeing it.
 */
static char* getScopedName_thunk( CXCursor cursor, getCursorName_fn name_fn ) {
  assert( name_fn != NULL );

  CXCursor const ref_csr = clang_getCursorReferenced( cursor );
  if ( !tidy_Cursor_isInvalid( ref_csr ) )
    cursor = ref_csr;

  strbuf_t sbuf = STRBUF_INIT();
  getScopedName_impl( cursor, name_fn, &sbuf );
  return strbuf_take( &sbuf );
}

////////// extern functions ///////////////////////////////////////////////////

bool tidy_Cursor_callsBaseConstructor( CXCursor ctor_csr,
                                       CXCursor base_cls_csr ) {
  callsBaseConstructor_data cbcd = {
    .base_cls_csr = clang_getCanonicalCursor( base_cls_csr )
  };
  clang_visitChildren( ctor_csr, &callsBaseConstructor_visitor, &cbcd );
  return cbcd.found_explicit_call;
}

bool tidy_Cursor_getConstructorInfo( CXCursor cursor,
                                     tidy_ConstructorInfo *ctor_info ) {
  if ( tidy_Cursor_isInvalid( cursor ) )
    return false;

  CXType type = clang_getCursorType( cursor );
         type = clang_getCanonicalType( type );

  CXCursor cls_csr = clang_getTypeDeclaration( type );
  if ( tidy_Cursor_isInvalid( cls_csr ) )
    cls_csr = cursor;

  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return false;

  tidy_ConstructorInfo local_info;
  if ( ctor_info == NULL )
    ctor_info = &local_info;

  *ctor_info = (tidy_ConstructorInfo){
    .default_ctor_csr = clang_getNullCursor()
  };
  clang_visitChildren( cls_csr, &getConstructorInfo_visitor, ctor_info );

  bool has_ctor =
    !clang_Cursor_isNull( ctor_info->default_ctor_csr ) ||
    ctor_info->has_other_ctors;

  // Fall back to template declaration if cls_csr is a template specialization.
  if ( !has_ctor ) {
    CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( cls_csr );
    if ( !tidy_Cursor_isInvalid( tmpl_csr ) )
      clang_visitChildren( tmpl_csr, &getConstructorInfo_visitor, ctor_info );
  }

  has_ctor =
    !clang_Cursor_isNull( ctor_info->default_ctor_csr ) ||
    ctor_info->has_other_ctors;
  return has_ctor;
}

CXCursor tidy_Cursor_getDefaultConstructor( CXCursor cls_csr ) {
  cls_csr = clang_getCanonicalCursor( cls_csr );
  CXCursor default_ctor_csr = clang_getNullCursor();
  clang_visitChildren(
    cls_csr, &getDefaultConstructor_visitor, &default_ctor_csr
  );
  return default_ctor_csr;
}

CXCursor tidy_Cursor_getDestructor( CXCursor cls_csr ) {
  if ( tidy_Cursor_isInvalid( cls_csr ) )
    return cls_csr;
  cls_csr = clang_getCanonicalCursor( cls_csr );
  CXCursor dtor_csr = clang_getNullCursor();
  clang_visitChildren( cls_csr, &getDestructor_visitor, &dtor_csr );
  return dtor_csr;
}

CXCursor tidy_Cursor_getElementIfArray( CXCursor cursor ) {
  if ( tidy_Cursor_isInvalid( cursor ) )
    return cursor;

  CXType type = clang_getCursorType( cursor );
         type = clang_getCanonicalType( type );

  bool is_array = false;
  for ( bool done = false; !done; ) {
    switch ( type.kind ) {
      case CXType_ConstantArray:
      case CXType_DependentSizedArray:
      case CXType_IncompleteArray:
      case CXType_VariableArray:
        type = clang_getElementType( type );
        type = clang_getCanonicalType( type );
        is_array = true;
        break;
      default:
        done = true;
        break;
    }
  } // for

  if ( is_array ) {
    CXCursor const elem_csr = clang_getTypeDeclaration( type );
    if ( !tidy_Cursor_isInvalid( elem_csr ) )
      return elem_csr;
  }

  return cursor;
}

char* tidy_Cursor_getScopedDisplayName( CXCursor cursor ) {
  return getScopedName_thunk( cursor, &clang_getCursorDisplayName );
}

char* tidy_Cursor_getScopedSpelling( CXCursor cursor ) {
  return getScopedName_thunk( cursor, &clang_getCursorSpelling );
}

bool tidy_Cursor_isClassDecl( CXCursor cursor ) {
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_UnionDecl:
      return true;
    default:
      return false;
  } // switch
}

bool tidy_Cursor_isEnclosingScope( CXCursor cursor ) {
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_Constructor:
    case CXCursor_ConversionFunction:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_FunctionDecl:
    case CXCursor_FunctionTemplate:
    case CXCursor_Namespace:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      return true;
    default:
      return false;
  } // switch
}

bool tidy_Cursor_isExternalGlobalFunction( CXCursor cursor ) {
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_FunctionDecl )
    return false;
  CXCursor const parent = clang_getCursorSemanticParent( cursor );
  enum CXCursorKind const parent_kind = clang_getCursorKind( parent );
  if ( parent_kind != CXCursor_TranslationUnit )
    return false;
  if ( !tidy_Cursor_isExternalLinkage( cursor ) )
    return false;

  return true;
}

bool tidy_Cursor_isExternalLinkage( CXCursor cursor ) {
  enum CXLinkageKind const linkage = clang_getCursorLinkage( cursor );
  switch ( linkage ) {
    case CXLinkage_External:
    case CXLinkage_UniqueExternal:
      return true;
    default:
      return false;
  } // switch
}

bool tidy_Cursor_isReservedName( CXCursor cursor, enum CXLanguageKind lang ) {
  bool              is_reserved = false;
  CXString const    name_cxs = clang_getCursorSpelling( cursor );
  char const *const name = clang_getCString( name_cxs );

  if ( name == NULL || name[0] == '\0' )
    goto done;                          // LCOV_EXCL_LINE

  if ( name[0] == '_' ) {
    if ( name[1] == '_' || isupper( STATIC_CAST( unsigned char, name[1] ) ) ) {
      is_reserved = true;
    }
    else {
      switch ( lang ) {
        case CXLanguage_C:;
          enum CXLinkageKind const linkage = clang_getCursorLinkage( cursor );
          if ( linkage == CXLinkage_External )
            is_reserved = true;
          break;
        case CXLanguage_CPlusPlus:;
          CXCursor const parent = clang_getCursorSemanticParent( cursor );
          enum CXCursorKind const kind = clang_getCursorKind( parent );
          if ( kind == CXCursor_TranslationUnit )
            is_reserved = true;
          break;
        default:
          /* suppress warning */;
      } // switch
    }
  }
  else if ( lang == CXLanguage_CPlusPlus && strstr( name, "__" ) != NULL ) {
    is_reserved = true;
  }

done:
  clang_disposeString( name_cxs );
  return is_reserved;
}

bool tidy_Cursor_isScopeDecl( CXCursor cursor ) {
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_EnumDecl:
    case CXCursor_Namespace:
    case CXCursor_StructDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_UnionDecl:
      return true;
    default:
      return false;
  } // switch
}

bool tidy_Cursor_isSpellingEqualTo( CXCursor cursor, char const *value ) {
  assert( value != NULL );

  CXString const    cxs = clang_getCursorSpelling( cursor );
  char const *const cs = clang_getCString( cxs );
  int const         cmp = cs == NULL ? 1 : strcmp( cs, value );

  clang_disposeString( cxs );
  return cmp == 0;
}

int tidy_Cursor_isSpellingEqualToAny_impl( CXCursor cursor,
                                           char const *values[] ) {
  assert( values != NULL );

  CXString const    cxs = clang_getCursorSpelling( cursor );
  char const *const cs = clang_getCString( cxs );

  int equal_idx = -1;

  for ( int i = 0; values[i] != NULL; ++i ) {
    if ( strcmp( cs, values[i] ) == 0 ) {
      equal_idx = i;
      break;
    }
  } // for

  clang_disposeString( cxs );
  return equal_idx;
}

CXString tidy_File_getRealPathName( CXFile file ) {
  assert( file != NULL );

  CXString          abs_path_cxs = clang_File_tryGetRealPathName( file );
  char const *const abs_path = clang_getCString( abs_path_cxs );

  if ( unlikely( abs_path == NULL || abs_path[0] == '\0' ) ) {
    // LCOV_EXCL_START
    clang_disposeString( abs_path_cxs );
    abs_path_cxs = clang_getFileName( file );
    // LCOV_EXCL_STOP
  }

  return abs_path_cxs;
}

CXCursor tidy_getCursorByName( char const *name, CXCursor scope_csr ) {
  assert( name != NULL );

  getCursorByName_data gcbnd = {
    .find_name = name,
    .found_csr = clang_getNullCursor(),
    .skip_csr = clang_getNullCursor()
  };

  while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
    clang_visitChildren( scope_csr, &getCursorByName_visitor, &gcbnd );
    if ( !clang_Cursor_isNull( gcbnd.found_csr ) )
      return gcbnd.found_csr;
    enum CXCursorKind const kind = clang_getCursorKind( scope_csr );
    if ( kind == CXCursor_TranslationUnit )
      break;
    enum CXLanguageKind const lang = clang_getCursorLanguage( scope_csr );
    gcbnd.cxx_recurse_into_scope = lang == CXLanguage_CPlusPlus;
    gcbnd.skip_csr = scope_csr;
    scope_csr = clang_getCursorSemanticParent( scope_csr );
  } // while

  return (CXCursor){ .kind = CXCursor_NoDeclFound };
}

CXCursor tidy_getCursorByNameToken( CXTranslationUnit tu, CXToken token,
                                    CXCursor scope_csr ) {
  if ( clang_getTokenKind( token ) != CXToken_Identifier )
    return clang_getNullCursor();       // LCOV_EXCL_LINE

  CXString const    token_cxs = clang_getTokenSpelling( tu, token );
  char const *const token_cs = clang_getCString( token_cxs );
  CXCursor const    rv_csr = tidy_getCursorByName( token_cs, scope_csr );

  clang_disposeString( token_cxs );
  return rv_csr;
}

CXSourceRange tidy_getCursorExtent( CXCursor cursor ) {
  CXSourceRange const range = clang_getCursorExtent( cursor );
  CXSourceLocation    start_loc = clang_getRangeStart( range );
  CXSourceLocation    end_loc = clang_getRangeEnd( range );

  CXFile start_file, end_file;
  unsigned start_offset, end_offset;

  clang_getFileLocation(
    start_loc, &start_file, /*line=*/NULL, /*column=*/NULL, &start_offset
  );
  clang_getFileLocation(
    end_loc, &end_file, /*line=*/NULL, /*column=*/NULL, &end_offset
  );

  if ( start_file == NULL || start_file != end_file )
    return range;                       // LCOV_EXCL_LINE

  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( cursor );

  start_loc = clang_getLocationForOffset( tu, start_file, start_offset );
  end_loc   = clang_getLocationForOffset( tu, end_file, end_offset );

  return clang_getRange( start_loc, end_loc );
}

CXFile tidy_getCursorLocation_File( CXCursor cursor ) {
  CXSourceLocation const  loc = clang_getCursorLocation( cursor );
  CXFile                  file = tidy_getSpellingLocation_File( loc );

  if ( file == NULL ) {
    //
    // If tidy_getSpellingLocation_File() returns a NULL file, it can mean that
    // the symbol was formed via preprocessor token pasting, e.g., foo_ ## bar.
    // Fall back to tidy_getFileLocation_File().
    //
    file = tidy_getFileLocation_File( loc );
  }

  return file;
}

char* tidy_getCursorUSR( CXCursor cursor ) {
  cursor = clang_getCanonicalCursor( cursor );
  CXString const usr_cxs = clang_getCursorUSR( cursor );
  char const *const usr_cs = clang_getCString( usr_cxs );
  char *usr = NULL;
  if ( usr_cs != NULL && usr_cs[0] != '\0' )
    usr = strdup_or_exit( usr_cs );
  clang_disposeString( usr_cxs );
  return usr;
}

bool tidy_isCursorDefinition( CXCursor cursor ) {
  if ( clang_isCursorDefinition( cursor ) )
    return true;
  enum CXLanguageKind const lang = clang_getCursorLanguage( cursor );
  if ( lang == CXLanguage_CPlusPlus )
    return false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_VarDecl )
    return false;
  enum CX_StorageClass const storage = clang_Cursor_getStorageClass( cursor );
  if ( storage == CX_SC_Extern )
    return false;
  return true;
}

bool tidy_isEscapedNewline( CXTranslationUnit tu, char const *source_text,
                            CXToken const *tokens, unsigned token_idx ) {
  assert( source_text != NULL );
  assert( tokens != NULL );
  assert( token_idx > 0 );

  CXSourceRange const prev_range =
    clang_getTokenExtent( tu, tokens[ token_idx - 1 ] );
  CXSourceRange const curr_range =
    clang_getTokenExtent( tu, tokens[ token_idx ] );
  CXSourceLocation const prev_loc = clang_getRangeEnd( prev_range );
  CXSourceLocation const curr_loc = clang_getRangeStart( curr_range );

  CXFile prev_file, curr_file;
  unsigned prev_offset, curr_offset;
  clang_getSpellingLocation(
    prev_loc, &prev_file, /*line=*/NULL, /*column=*/NULL, &prev_offset
  );
  clang_getSpellingLocation(
    curr_loc, &curr_file, /*line=*/NULL, /*column=*/NULL, &curr_offset
  );
  if ( prev_file == NULL || prev_file != curr_file )
    return false;

  bool saw_backslash = false;
  for ( unsigned i = prev_offset; i < curr_offset; ++i ) {
    switch ( source_text[i] ) {
      case '\\':
        saw_backslash = true;
        break;
      case '\n':
      case '\r':
        if ( !saw_backslash )
          return false;
        saw_backslash = false;
        break;
    } // switch
  } // for

  return true;
}

CXToken const* tidy_Token_getNext( CXToken const tokens[], unsigned token_count,
                                   unsigned *ptoken_idx ) {
  assert( ptoken_idx != NULL );

  CXToken const *pnext_token = NULL;
  unsigned i;

  for ( i = *ptoken_idx + 1; i < token_count; ++i ) {
    CXTokenKind const kind = clang_getTokenKind( tokens[i] );
    if ( kind != CXToken_Comment ) {
      pnext_token = &tokens[i];
      break;
    }
  } // for

  *ptoken_idx = i;
  return pnext_token;
}

CXCursor tidy_Token_getScopedNameCursor( CXTranslationUnit tu,
                                         CXToken const tokens[],
                                         unsigned token_count,
                                         unsigned *ptoken_idx,
                                         CXCursor scope_csr,
                                         enum CXLanguageKind lang ) {
  assert( ptoken_idx != NULL );
  unsigned token_idx = *ptoken_idx;

  CXCursor rv_csr =
    tidy_getCursorByNameToken( tu, tokens[ *ptoken_idx ], scope_csr );

  if ( lang == CXLanguage_CPlusPlus ) {
    while ( !tidy_Cursor_isInvalid( rv_csr ) ) {
      unsigned next_idx = token_idx;
      CXToken const *t;

      // Look for "::".
      if ( (t = tidy_Token_getNext( tokens, token_count, &next_idx )) == NULL )
        break;
      if ( !tidy_Token_isScopeQualifier( tu, *t ) )
        break;

      // Look for an identifier.
      if ( (t = tidy_Token_getNext( tokens, token_count, &next_idx )) == NULL )
        break;
      CXCursor const next_csr = tidy_getCursorByNameToken( tu, *t, rv_csr );
      if ( tidy_Cursor_isInvalid( next_csr ) )
        break;                          // LCOV_EXCL_LINE

      rv_csr = next_csr;
      token_idx = next_idx;
    } // while

    *ptoken_idx = token_idx;
  }

  return rv_csr;
}

bool tidy_Token_isScopeQualifier( CXTranslationUnit tu, CXToken token ) {
  if ( clang_getTokenKind( token ) != CXToken_Punctuation )
    return false;
  return tidy_Token_isSpellingEqualTo( tu, token, "::" );
}

bool tidy_Token_isSpellingEqualTo( CXTranslationUnit tu, CXToken token,
                                   char const *value ) {
  assert( value != NULL );

  CXString const    cxs = clang_getTokenSpelling( tu, token );
  char const *const cs = clang_getCString( cxs );
  int const         cmp = strcmp( cs, value );

  clang_disposeString( cxs );
  return cmp == 0;
}

int tidy_Token_isSpellingEqualToAny_impl( CXTranslationUnit tu, CXToken token,
                                          char const *values[] ) {
  assert( values != NULL );

  CXString const    cxs = clang_getTokenSpelling( tu, token );
  char const *const cs = clang_getCString( cxs );

  int equal_idx = -1;

  for ( int i = 0; values[i] != NULL; ++i ) {
    if ( strcmp( cs, values[i] ) == 0 ) {
      equal_idx = i;
      break;
    }
  } // for

  clang_disposeString( cxs );
  return equal_idx;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/// @cond DOXYGEN_IGNORE

extern inline bool tidy_Cursor_isInvalid( CXCursor );
extern inline CXFile tidy_getFileLocation_File( CXSourceLocation );
extern inline CXFile tidy_getSpellingLocation_File( CXSourceLocation );
extern inline unsigned tidy_getSpellingLocation_Line( CXSourceLocation );
extern inline unsigned tidy_getTokenLocation_Line( CXTranslationUnit, CXToken );

/// @endcond

/* vim:set et sw=2 ts=2: */
