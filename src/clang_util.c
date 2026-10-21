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

typedef struct getCursorByName_data getCursorByName_data;
typedef CXString                  (*getCursorName_fn)( CXCursor );
typedef struct isBaseClass_data     isBaseClass_data;

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data passed to getCursorByName_visitor().
 */
struct getCursorByName_data {
  char const *find_name;                ///< The name to find.
  CXCursor    found_csr;                ///< The name's cursor, if found.
  CXCursor    skip_csr;                 ///< Skip this cursor.
  bool        cxx_recurse_into_scope;   ///< C++: recurse into scope?
};

/**
 * Additional data passed to isBaseClass_visitor().
 */
struct isBaseClass_data {
  /**
   * A candidate base class.
   *
   * @note This _must_ be set to a canonical cursor.
   */
  CXCursor base_csr;

  /**
   * Set to `true` only if \ref base_csr is a base class of a given cursor.
   */
  bool is_base;
};

////////// local functions ////////////////////////////////////////////////////

/**
 * A helper function for tidy_getCursorByName() that Visits each symbol within
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

  if ( clang_isDeclaration( kind ) || kind == CXCursor_MacroDefinition ) {
    CXString const    name_cxs = clang_getCursorSpelling( cursor );
    char const *const name_cs = clang_getCString( name_cxs );
    bool const        is_found = strcmp( name_cs, gcbnd->find_name ) == 0;

    clang_disposeString( name_cxs );

    if ( is_found ) {
      gcbnd->found_csr = cursor;
      return CXChildVisit_Break;
    }
  }

  switch ( kind ) {
    case CXCursor_CXXBaseSpecifier:;
      CXCursor base_csr = clang_getCursorReferenced( cursor );
      if ( tidy_Cursor_isInvalid( base_csr ) ) {
        // LCOV_EXCL_START
        CXType const type = clang_getCursorType( cursor );
        base_csr = clang_getTypeDeclaration( type );
        // LCOV_EXCL_STOP
      }
      if ( !tidy_Cursor_isInvalid( base_csr ) )
        clang_visitChildren( base_csr, &getCursorByName_visitor, data );
      break;

    case CXCursor_Namespace:
      if ( gcbnd->cxx_recurse_into_scope )
        return CXChildVisit_Recurse;
      break;

    default:
      /* suppress warning */;
  } // switch

skip:
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

/**
 * A helper function for tidy_Cursor_isInheritedFrom() that visits the children
 * of a class looking for a base class.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The isBaseClass_data to use.
 * @return Returns `CXChildVisit_Break` only if a base class is found;
 * otherwise returns `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult isBaseClass_visitor( CXCursor cursor,
                                                    CXCursor parent,
                                                    CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind == CXCursor_CXXBaseSpecifier ) {
    CXCursor ref_csr = clang_getCursorReferenced( cursor );
    if ( tidy_Cursor_isInvalid( ref_csr ) ) {
      // LCOV_EXCL_START
      CXType const type = clang_getCursorType( cursor );
      ref_csr = clang_getTypeDeclaration( type );
      // LCOV_EXCL_STOP
    }
    ref_csr = clang_getCanonicalCursor( ref_csr );

    assert( data != NULL );
    isBaseClass_data *const ibcd = data;

    if ( clang_equalCursors( ref_csr, ibcd->base_csr ) ||
         tidy_Cursor_isInheritedFrom( ref_csr, ibcd->base_csr ) ||
         tidy_Cursor_isTemplateSpecializationOf( ibcd->base_csr, ref_csr ) ) {
      ibcd->is_base = true;
      return CXChildVisit_Break;
    }
  }

  return CXChildVisit_Continue;
}

/**
 * Gets whether the kind of \a cursor can be inherited from a base class into a
 * derived class.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is inheritable.
 */
NODISCARD
static bool tidy_Cursor_isInheritable( CXCursor cursor ) {
  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    // data members
    case CXCursor_FieldDecl:
    case CXCursor_VarDecl:              // static data members
    // member functions
    case CXCursor_CXXMethod:
    case CXCursor_ConversionFunction:
    case CXCursor_FunctionTemplate:
    // types
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
      return true;
    default:
      return false;
  } // switch
}

////////// extern functions ///////////////////////////////////////////////////

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

bool tidy_Cursor_isInheritedFrom( CXCursor cursor, CXCursor base_csr ) {
  if ( tidy_Cursor_isInvalid( cursor ) || tidy_Cursor_isInvalid( base_csr ) )
    return false;

  if ( tidy_Cursor_isInheritable( base_csr ) )
    base_csr = clang_getCursorSemanticParent( base_csr );
  if ( !tidy_Cursor_isClassDecl( base_csr ) )
    return false;
  base_csr = clang_getCanonicalCursor( base_csr );

  while ( !tidy_Cursor_isInvalid( cursor ) ) {
    enum CXCursorKind const kind = clang_getCursorKind( cursor );
    if ( clang_isTranslationUnit( kind ) )
      break;

    if ( tidy_Cursor_isClassDecl( cursor ) ) {
      if ( tidy_Cursor_isTemplateSpecializationOf( base_csr, cursor ) )
        return true;
      isBaseClass_data ibcd = { .base_csr = base_csr };
      clang_visitChildren( cursor, &isBaseClass_visitor, &ibcd );
      if ( ibcd.is_base )
        return true;
    }

    CXCursor parent = clang_getCursorSemanticParent( cursor );
    if ( unlikely( tidy_Cursor_isInvalid( parent ) ||
                   clang_equalCursors( parent, cursor ) ) ) {
      // LCOV_EXCL_START
      parent = clang_getCursorLexicalParent( cursor );
      // LCOV_EXCL_STOP
    }
    cursor = parent;
  } // while

  return false;
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

bool tidy_Cursor_isTemplateSpecializationOf( CXCursor cursor,
                                             CXCursor template_csr ) {
  template_csr = clang_getSpecializedCursorTemplate( template_csr );
  if ( !tidy_Cursor_isInvalid( template_csr ) ) {
    template_csr = clang_getCanonicalCursor( template_csr );
    if ( clang_equalCursors( cursor, template_csr ) )
      return true;
  }
  return false;
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

bool tidy_Token_isEqualTo( CXTranslationUnit tu, CXToken token,
                           char const *value ) {
  assert( value != NULL );

  CXString const    token_cxs = clang_getTokenSpelling( tu, token );
  char const *const token_cs = clang_getCString( token_cxs );
  int const         cmp = strcmp( token_cs, value );

  clang_disposeString( token_cxs );
  return cmp == 0;
}

int tidy_Token_isEqualToAny_impl( CXTranslationUnit tu, CXToken token,
                                  char const *values[] ) {
  assert( values != NULL );

  CXString const    token_cxs = clang_getTokenSpelling( tu, token );
  char const *const token_cs = clang_getCString( token_cxs );

  int equal_idx = -1;

  for ( int i = 0; values[i] != NULL; ++i ) {
    if ( strcmp( token_cs, values[i] ) == 0 ) {
      equal_idx = i;
      break;
    }
  } // for

  clang_disposeString( token_cxs );
  return equal_idx;
}

bool tidy_Token_isScopeQualifier( CXTranslationUnit tu, CXToken token ) {
  if ( clang_getTokenKind( token ) != CXToken_Punctuation )
    return false;
  return tidy_Token_isEqualTo( tu, token, "::" );
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
