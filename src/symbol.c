/*
**      symbol-tidy -- Symbol tidier
**      src/symbol.c
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
 * Defines structures and functions for keeping track of symbols referenced.
 */

// local
#include "pjl_config.h"
#include "symbol.h"
#include "array.h"
#include "clang_util.h"
#include "color.h"
#include "config_file.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "str_util.h"
#include "symbol-tidy.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>                     /* for unreachable(3) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup tidy-symbols-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
typedef struct svd_cxx_constructor  svd_cxx_constructor;
typedef struct svd_cxx_for_range    svd_cxx_for_range;
typedef struct symbols_visit_data   symbols_visit_data;
/// @endcond

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data passed to symbols_visitor().
 */
struct symbols_visit_data {
  tidy_source const  *source;           ///< Source file being tidied.
  tidy_symbol        *parent_sym;       ///< Current parent symbol.

  /**
   * Additional data for visit_Constructor().
   */
  struct svd_cxx_constructor {
    CXCursor          ctor_csr;         ///< Constructor's cursor.
  } cxx_constructor;

  /**
   * Additional data for visit_CXXForRangeStmt().
   */
  struct svd_cxx_for_range {
    bool  found_begin;                  ///< Found `begin()`?
    bool  found_end;                    ///< Found `end()`?
    bool  is_const;                     ///< Is the range-for `const`?
  } cxx_for_range;
};

////////// local functions ////////////////////////////////////////////////////

static tidy_symbol* add_symbol( CXCursor, tidy_source const* );

static void symbol_add_ref( tidy_symbol*, tidy_symbol* );
static void symbol_cleanup( tidy_symbol* );

NODISCARD
static bool symbol_is_excluded( CXCursor, tidy_source const* );

static void visit_Constructor( CXCursor, CXCursor, symbols_visit_data* );
static void visit_CXXForRangeStmt( CXCursor, CXCursor, symbols_visit_data* );
static void visit_CXXMethod( CXCursor, CXCursor, symbols_visit_data* );
static void visit_FieldDecl( CXCursor, CXCursor, symbols_visit_data* );
static void visit_MacroDefinition( CXCursor, CXCursor, symbols_visit_data* );
static void visit_most_Ref( CXCursor, CXCursor, symbols_visit_data* );
static void visit_OverloadedDeclRef( CXCursor, CXCursor,
                                     symbols_visit_data* );

static enum CXChildVisitResult visit_Constructor_vistor( CXCursor, CXCursor,
                                                         CXClientData );

////////// local variables ////////////////////////////////////////////////////

static hash_table_t symbol_set;         ///< Set of symbols.

////////// local functions ////////////////////////////////////////////////////

/**
 * A helper function for visit_CXXForRangeStmt() that looks for `begin()` and
 * `end()` member functions, and, if found, adds references to them.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data A pointer to receive the cursor for the range expression.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult add_begin_end_ref_visitor( CXCursor cursor,
                                                          CXCursor parent,
                                                          CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( kind != CXCursor_CXXMethod )
    goto skip;
  int const num_args = clang_Cursor_getNumArguments( cursor );
  if ( num_args > 0 )
    goto skip;

  assert( data != NULL );
  symbols_visit_data *const svd = data;

  bool const is_method_const = clang_CXXMethod_isConst( cursor );
  if ( is_method_const != svd->cxx_for_range.is_const )
    goto skip;

  CXString const    name_cxs = clang_getCursorSpelling( cursor );
  char const *const name = clang_getCString( name_cxs );

  if ( name == NULL )
    goto done;
  if ( strcmp( name, "begin" ) == 0 )
    svd->cxx_for_range.found_begin = true;
  else if ( strcmp( name, "end" ) == 0 )
    svd->cxx_for_range.found_end = true;
  else
    goto done;

  tidy_symbol *const sym = add_symbol( cursor, svd->source );
  if ( sym != NULL )
    symbol_add_ref( svd->parent_sym, sym );

done:
  clang_disposeString( name_cxs );

  if ( svd->cxx_for_range.found_begin && svd->cxx_for_range.found_end )
    return CXChildVisit_Break;

skip:
  return CXChildVisit_Continue;
}

/**
 * Adds a symbol to symbol_set.
 *
 * @param sym_csr The cursor for the symbol to add.
 * @param source The tidy_source to use.
 * @return Returns the new tidy_symbol or NULL if it could not be added.
 */
static tidy_symbol* add_symbol( CXCursor sym_csr, tidy_source const *source ) {
  if ( symbol_is_excluded( sym_csr, source ) )
    return NULL;
  char *key = tidy_getCursorUSR( sym_csr );
  if ( unlikely( key == NULL ) )
    return NULL;

  tidy_symbol  *sym = NULL;
  char         *sym_name = tidy_Cursor_getScopedSpelling( sym_csr );

  if ( sym_name == NULL )
    goto done;
  if ( strcmp( sym_name, TIDY_COMPILER_ID ) == 0 )
    goto done;

  bool is_sym_ignored = false;
  CXFile const sym_file = tidy_getCursorLocation_File( sym_csr );
  if ( sym_file != NULL ) {
    CXString const    sym_path_cxs = clang_getFileName( sym_file );
    char const *const sym_path_cs = clang_getCString( sym_path_cxs );
    is_sym_ignored = config_symbol_is_ignored( sym_name, sym_path_cs );
    clang_disposeString( sym_path_cxs );
  }
  if ( is_sym_ignored )
    goto done;

  tidy_symbol new_sym = { .key = key, .name = sym_name };
  key = NULL;                           // new_sym owns this now
  sym_name = NULL;                      // new_sym owns this now
  ht_insert_rv_t const hti =
    ht_table_insert( &symbol_set, &new_sym, sizeof new_sym );
  sym = HT_DINT( hti.entry );

  if ( !hti.inserted ) {
    symbol_cleanup( &new_sym );
    goto done;
  }

  if ( source->lang == CXLanguage_CPlusPlus )
    sym->display_name = tidy_Cursor_getScopedDisplayName( sym_csr );

  //
  // If the symbol's name is "main", we have to mark it as a root since nothing
  // calls main().  But first ensure that it's the program's main() function:
  // in C, anything named "main" must be main(); however, in C++, other things,
  // e.g., member functions, can also be named "main".
  //
  if ( strcmp( sym->name, "main" ) == 0 &&
       tidy_Cursor_isExternalGlobalFunction( sym_csr ) ) {
    sym->is_root = true;
  }

done:
  free( key );
  free( sym_name );
  return sym;
}

/**
 * A helper function for visit_CXXForRangeStmt() that attempts to find the
 * range expression of a range-for statement.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data A pointer to receive the cursor for the range expression.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult get_Expr_visitor( CXCursor cursor,
                                                 CXCursor parent,
                                                 CXClientData data ) {
  (void)parent;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  if ( clang_isExpression( kind ) ) {
    assert( data != NULL );
    CXCursor *const expr_csr = data;
    *expr_csr = cursor;
    return CXChildVisit_Break;
  }

  return CXChildVisit_Continue;
}

/**
 * Gets whether \a source has an escaped newline between \a tokens \a token_idx
 * - 1 and \a token_idx.
 *
 * @param source The source to check.
 * @param tokens The tokens for \a source.
 * @param token_idx The current index into \a tokens.
 * @return Returns `true` only if there's an escaped newline.
 */
NODISCARD
static bool is_escaped_newline( tidy_source const *source,
                                CXToken const *tokens, unsigned token_idx ) {
  assert( source != NULL );
  assert( tokens != NULL );
  assert( token_idx > 0 );

  CXSourceRange const prev_range =
    clang_getTokenExtent( source->tu, tokens[ token_idx - 1 ] );
  CXSourceRange const curr_range =
    clang_getTokenExtent( source->tu, tokens[ token_idx ] );
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
    switch ( source->text[i] ) {
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

/**
 * For a macro, gets the cursor for the identifier given by \a token within \a
 * scope_csr, but only if \a token actually is an identifier, neither
 * `__VA_ARGS__` nor `__VA_OPT__`, nor one of the current macro's parameters.
 *
 * @remarks This is a variant of tidy_getCursorByNameToken(), but for a macro
 * that additionally takes \a param_set.
 *
 * @param tu The translation unit to use.
 * @param token The token to get the cursor for.
 * @param scope_csr The cursor of the scope to search within.
 * @param param_set The set of macro parameter names.
 * @return Returns said cursor; or an invalid cursor if \a token is an
 * identifier, but not found; or the null cursor if \a token is:
 *  + Not an identifier; or:
 *  + Either `__VA_ARGS__` nor `__VA_OPT__`; or:
 *  + In \a param_set.
 *
 * @sa tidy_getCursorByNameToken()
 */
NODISCARD
static CXCursor macro_getCursorByNameToken( CXTranslationUnit tu,
                                            CXToken token, CXCursor scope_csr,
                                            hash_table_t const *param_set ) {
  assert( param_set != NULL );

  if ( clang_getTokenKind( token ) != CXToken_Identifier )
    return clang_getNullCursor();

  CXString const    token_cxs = clang_getTokenSpelling( tu, token );
  char const *const token_cs = clang_getCString( token_cxs );

  CXCursor const rv_csr =
    strcmp( token_cs, "__VA_ARGS__" ) != 0 &&
    strcmp( token_cs, "__VA_OPT__" ) != 0 &&
    ht_table_find( param_set, token_cs ) == NULL ?
      tidy_getCursorByName( token_cs, scope_csr )
    :
      clang_getNullCursor();

  clang_disposeString( token_cxs );
  return rv_csr;
}

/**
 * Gets the names of all of a macro's parameters.
 *
 * @param tu The translation unit to use.
 * @param tokens The array of macro tokens.
 * @param token_count The length of \a tokens.
 * @param param_set The set to add the parameter names to.
 * @return Returns the index of the token one past the `)`.
 */
NODISCARD
static unsigned macro_get_params( CXTranslationUnit tu,
                                  CXToken const tokens[static 2],
                                  unsigned token_count,
                                  hash_table_t *param_set ) {
  assert( param_set != NULL );

  if ( unlikely( token_count < 2 ) )
    return 1;

  unsigned rv_idx = 1;

  // Start at index 2 since tokens[0] is the macro name, tokens[1] is the '('.
  for ( unsigned i = 2; i < token_count; ++i ) {
    CXTokenKind const kind = clang_getTokenKind( tokens[i] );
    switch ( kind ) {
      case CXToken_Identifier:
      case CXToken_Punctuation:
        break;
      default:
        continue;
    } // switch

    CXString const    token_cxs = clang_getTokenSpelling( tu, tokens[i] );
    char const *const token_cs = clang_getCString( token_cxs );

    switch ( kind ) {
      case CXToken_Identifier:
        ht_table_insert(
          param_set, CONST_CAST( char*, token_cs ), strlen( token_cs ) + 1/*\0*/
        );
        break;
      case CXToken_Punctuation:
        if ( strcmp( token_cs, ")" ) == 0 ) {
          rv_idx = i + 1;
          i = token_count;              // will cause loop to exit
        }
        break;
      default:
        unreachable();
    } // switch

    clang_disposeString( token_cxs );
  } // for

  return rv_idx;
}

/**
 * For a macro, gets the cursor for the scoped symbol from \a tokens.
 *
 * @remarks This is a variant of tidy_Token_getScopedNameCursor(), but for a
 * macro that additionally takes \a param_set.
 *
 * @param tu The translation unit to use.
 * @param tokens The array of macro tokens.
 * @param token_count The length of \a tokens.
 * @param ptoken_idx A pointer to the current index within \a tokens.
 * @param param_set The set of macro parameter names.
 * @param lang The language of the source file being tidied.
 * @return Returns said cursor or the null cursor for none.
 *
 * @sa tidy_Token_getScopedNameCursor()
 */
NODISCARD
static
CXCursor macro_Token_getScopedNameCursor( CXTranslationUnit tu,
                                          CXToken const tokens[],
                                          unsigned token_count,
                                          unsigned *ptoken_idx,
                                          hash_table_t const *param_set,
                                          enum CXLanguageKind lang ) {
  assert( param_set != NULL );
  unsigned token_idx = *ptoken_idx;

  CXCursor const tu_csr = clang_getTranslationUnitCursor( tu );
  CXCursor rv_csr =
    macro_getCursorByNameToken( tu, tokens[ *ptoken_idx ], tu_csr, param_set );

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
      CXCursor const next_csr =
        macro_getCursorByNameToken( tu, *t, rv_csr, param_set );
      if ( tidy_Cursor_isInvalid( next_csr ) )
        break;                          // LCOV_EXCL_LINE

      rv_csr = next_csr;
      token_idx = next_idx;
    } // while

    *ptoken_idx = token_idx;
  }

  return rv_csr;
}

/**
 * Prints statistics for symbols if requested.
 */
static void print_statistics( void ) {
  if ( !verbose_print_statistics() )
    return;

  verbose_printf(
    "  symbol set:\n"
  );
  verbose_printf(
    "    ss-load-factor = " TIDY_STAT_LF_FMT "\n",
                          ht_table_load_factor( &symbol_set )
  );
  verbose_printf(
    "    ss-size = %u\n", symbol_set.size
  );
}

/**
 * Adds a reference from \a from_sym to \a to_sym.
 *
 * @param from_sym The symbol to adda reference from.
 * @param to_sym The symbol to adda reference to.
 */
static void symbol_add_ref( tidy_symbol *from_sym, tidy_symbol *to_sym ) {
  assert( from_sym != NULL );
  assert( to_sym != NULL );

  for ( tidy_sym_ref const *ref = from_sym->refs; ref != NULL;
        ref = ref->next ) {
    if ( ref->sym == to_sym )
      return;
  } // for

  tidy_sym_ref *const ref = MALLOC( tidy_sym_ref, 1 );
  *ref = (tidy_sym_ref){ .sym = to_sym, .next = from_sym->refs };
  from_sym->refs = ref;
}

/**
 * Cleans-up a tidy_symbol.
 *
 * @param sym The tidy_symbol to clean up.  If NULL, does nothing.
 */
static void symbol_cleanup( tidy_symbol *sym ) {
  if ( sym != NULL ) {
    FREE( sym->key );
    FREE( sym->name );
    FREE( sym->display_name );
    FREE( sym->path );

    for ( tidy_sym_ref *ref = sym->refs, *next; ref != NULL; ref = next ) {
      next = ref->next;
      free( ref );
    } // for
  }
}

/**
 * Compares two \ref tidy_symbol objects by their source path, then by line,
 * then by column.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the first
 * symbol's path is less than, equal to, or greater than the second symbol's
 * path, respectively.
 */
NODISCARD
static int symbol_cmp_by_path( tidy_symbol const *i_sym,
                               tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( i_sym->path != NULL );
  assert( j_sym != NULL );
  assert( j_sym->path != NULL );

  int const cmp = strcmp( i_sym->path, j_sym->path );
  if ( cmp != 0 )
    return cmp;
  if ( i_sym->line < j_sym->line ) return -1;
  if ( i_sym->line > j_sym->line ) return  1;
  if ( i_sym->col  < j_sym->col  ) return -1;
  if ( i_sym->col  > j_sym->col  ) return  1;
  return 0;
}

/**
 * Gets whether \a sym_csr should be excluded from the global set.
 *
 * @param sym_csr The symbol's cursor to check.
 * @param source The tidy_source to use.
 * @return Returns `true` only if \a sym_csr is excluded.
 */
NODISCARD
static bool symbol_is_excluded( CXCursor sym_csr, tidy_source const *source ) {
  if ( tidy_Cursor_isInvalid( sym_csr ) )
    return true;

  assert( source != NULL );

  //
  // We can't use clang_getCursorLanguage() because libclang doesn't initially
  // set what it thinks the current language is until it encounters something
  // language-specific.  So just use source->lang.
  //
  if ( tidy_Cursor_isReservedName( sym_csr, source->lang ) )
    return true;

  enum CXCursorKind const sym_kind = clang_getCursorKind( sym_csr );
  switch ( sym_kind ) {
    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_TemplateTemplateParameter:
    case CXCursor_TemplateTypeParameter:
      //
      // These are local to the template definition, so don't need to be added
      // to symbol_set.
      //
      return true;

    case CXCursor_ParmDecl:
      //
      // These are local to a function, so don't need to be added to
      // symbol_set.
      //
      return true;

    default:
      return false;
  } // switch
}

/**
 * Marks a symbol as used.
 *
 * @param sym The symbol to mark as used.
 */
static void symbol_mark_used( tidy_symbol *sym ) {
  assert( sym != NULL );

  if ( !sym->is_used ) {
    sym->is_used = true;
    for ( tidy_sym_ref *ref = sym->refs; ref != NULL; ref = ref->next )
      symbol_mark_used( ref->sym );
  }
}

/**
 * Cleans-up all symbols.
 */
static void symbols_cleanup( void ) {
  print_statistics();
  ht_table_cleanup(
    &symbol_set, POINTER_CAST( ht_free_fn_t, &symbol_cleanup )
  );
}

/**
 * Marks all used symbols as used.
 *
 * @return Returns the number of unused symbols.
 */
static unsigned symbols_mark_used( void ) {
  ht_iterator_t iter;

  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_root )
      symbol_mark_used( sym );
  } // for

  unsigned unused = 0;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( !(sym->path == NULL || sym->line == 0 || sym->is_used) )
      ++unused;
  } // for

  return unused;
}

/**
 * Visits each symbol in a translation unit.
 *
 * @param cursor The cursor for the symbol in the AST being visited.
 * @param parent The parent cursor of \a cursor.
 * @param data A pointer to a symbols_visit_data.
 * @return Always returns `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult symbols_visitor( CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData data ) {
  (void)parent;

  CXSourceLocation const sym_loc = clang_getCursorLocation( cursor );
  if ( clang_Location_isInSystemHeader( sym_loc ) )
    goto skip;

  if ( IS_VERBOSE( CURSORS ) )
    verbose_print_cursor( cursor );     // LCOV_EXCL_LINE

  assert( data != NULL );
  symbols_visit_data *const svd = data;
  tidy_symbol *const prev_parent_sym = svd->parent_sym;

  bool is_definition = false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );

  switch ( kind ) {
    case CXCursor_CallExpr:

    case CXCursor_CXXNewExpr:
    case CXCursor_CXXDeleteExpr:
    case CXCursor_CXXFunctionalCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXDynamicCastExpr:

    case CXCursor_DeclRefExpr:
    case CXCursor_MacroExpansion:
    case CXCursor_MemberRefExpr:
    case CXCursor_TemplateRef:
    case CXCursor_TypeRef:
      visit_most_Ref( cursor, parent, svd );
      break;

    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_Constructor:
    case CXCursor_ConversionFunction:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_FunctionDecl:
    case CXCursor_VarDecl:
      is_definition = tidy_isCursorDefinition( cursor );
      break;

    case CXCursor_CXXForRangeStmt:
      visit_CXXForRangeStmt( cursor, parent, svd );
      break;

    case CXCursor_EnumConstantDecl:
    case CXCursor_MacroDefinition:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
      is_definition = true;
      break;

    case CXCursor_EnumDecl:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      if ( !clang_Cursor_isAnonymous( cursor ) )
        is_definition = true;
      break;

    case CXCursor_OverloadedDeclRef:
      visit_OverloadedDeclRef( cursor, parent, svd );
      break;

    default:
      /* suppress warning */;
  } // switch

  if ( is_definition ) {
    //
    // For a definition, look-up the symbol to update its definition location.
    //
    tidy_symbol *const sym = add_symbol( cursor, svd->source );
    if ( likely( sym != NULL ) ) {
      CXFile sym_file;
      clang_getSpellingLocation(
        sym_loc, &sym_file, &sym->line, &sym->col, /*offset=*/NULL
      );

      CXString const    path_cxs = clang_getFileName( sym_file );
      char const *const path = clang_getCString( path_cxs );
      if ( path != NULL && path[0] != '\0' && is_argv_path( path ) ) {
        FREE( sym->path );              // just in case
        sym->path = strdup_or_exit( path );
      }
      clang_disposeString( path_cxs );

      svd->parent_sym = sym;

      switch ( kind ) {
        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_ClassTemplatePartialSpecialization:
        case CXCursor_ConversionFunction:
        case CXCursor_Destructor:
        case CXCursor_EnumDecl:
        case CXCursor_FunctionDecl:
        case CXCursor_StructDecl:
        case CXCursor_UnionDecl:
          break;

        case CXCursor_Constructor:
          visit_Constructor( cursor, parent, svd );
          break;

        case CXCursor_CXXMethod:
          visit_CXXMethod( cursor, parent, svd );
          break;

        case CXCursor_FieldDecl:
          visit_FieldDecl( cursor, parent, svd );
          break;

        case CXCursor_MacroDefinition:
          visit_MacroDefinition( cursor, parent, svd );
          break;

        case CXCursor_VarDecl:
          //
          // If a variable of a class type is declared and the class has a
          // constructor, mark it as a root (used) since it may have side
          // effects.
          //
          if ( svd->source->lang == CXLanguage_CPlusPlus &&
               tidy_Cursor_hasConstructor( cursor ) ) {
            sym->is_root = true;
          }
          break;

        default:
          /* suppress warning */;
      } // switch
    }
  }

  clang_visitChildren( cursor, symbols_visitor, svd );
  svd->parent_sym = prev_parent_sym;

skip:
  return CXChildVisit_Continue;
}

/**
 * Gets the cursor for the scoped symbol from \a tokens.
 *
 * @param tu The translation unit to use.
 * @param tokens The array of macro tokens.
 * @param token_count The length of \a tokens.
 * @param ptoken_idx A pointer to the current index within \a tokens.
 * @param scope_csr The scope to look in.
 * @param lang The language of the source file being tidied.
 * @return Returns said cursor or the null cursor for none.
 */
NODISCARD
static CXCursor tidy_Token_getScopedNameCursor( CXTranslationUnit tu,
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
      CXCursor const next_csr =
        tidy_getCursorByNameToken( tu, *t, rv_csr );
      if ( tidy_Cursor_isInvalid( next_csr ) )
        break;                          // LCOV_EXCL_LINE

      rv_csr = next_csr;
      token_idx = next_idx;
    } // while

    *ptoken_idx = token_idx;
  }

  return rv_csr;
}

/**
 * Visits a `CXCursor_Constructor` kind of cursor.
 *
 * @param ctor_csr The constructor's cursor to visit.
 * @param parent The parent cursor of \a ctor_csr.
 * @param svd The symbols_visit_data to use.
 */
static void visit_Constructor( CXCursor ctor_csr, CXCursor parent,
                               symbols_visit_data *svd ) {
  (void)parent;

  CXCursor const cls_csr = clang_getCursorSemanticParent( ctor_csr );
  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    assert( svd != NULL );
    svd_cxx_constructor const prev_svd_cxx_constructor = svd->cxx_constructor;
    svd->cxx_constructor.ctor_csr = ctor_csr;
    clang_visitChildren( cls_csr, &visit_Constructor_vistor, svd );
    svd->cxx_constructor = prev_svd_cxx_constructor;

    //
    // Since destructors are called implicitly, they're not in the AST.
    // However, since a constructor was called, then its destructor must be
    // called, so add a reference to it.
    //
    CXCursor const dtor_csr = tidy_Cursor_getDestructor( cls_csr );
    if ( !tidy_Cursor_isInvalid( dtor_csr ) ) {
      tidy_symbol *const dtor_sym = add_symbol( dtor_csr, svd->source );
      if ( dtor_sym != NULL )
        symbol_add_ref( svd->parent_sym, dtor_sym );
    }
  }
}

/**
 * A helper function for visit_Constructor() that visits all base classes of \a
 * cursor looking for an explicit call to a base class's constructor: if none
 * is found, find the base class's default constructor and add a reference to
 * it.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 */
static enum CXChildVisitResult visit_Constructor_vistor( CXCursor cursor,
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

  assert( data != NULL );
  symbols_visit_data *const svd = data;
  if ( tidy_Cursor_callsBaseConstructor( svd->cxx_constructor.ctor_csr,
                                         base_cls_csr ) ) {
    goto skip;
  }

  // If no constructor of base_cls_csr was explicitly called, add a reference
  // to the default constructor.
  CXCursor const def_ctor = tidy_Cursor_getDefaultConstructor( base_cls_csr );
  if ( tidy_Cursor_isInvalid( def_ctor ) )
    goto skip;

  tidy_symbol *const def_ctor_sym = add_symbol( def_ctor, svd->source );
  if ( def_ctor_sym == NULL )
    goto skip;

  assert( svd->parent_sym != NULL );
  symbol_add_ref( svd->parent_sym, def_ctor_sym );

skip:
  //
  // We can't return CXChildVisit_Break even if we find either an explicit base
  // class constructor call or an implicit base class default constructor call
  // since there may be multiple base classes.  Hance, we always have to return
  // CXChildVisit_Continue.
  //
  return CXChildVisit_Continue;
}

/**
 * Visits a `CXCursor_CXXForRangeStmt` kind of cursor.
 *
 * @param range_stmt_csr The range-for statement's cursor.
 * @param parent Not used.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXForRangeStmt( CXCursor range_stmt_csr, CXCursor parent,
                                   symbols_visit_data *svd ) {
  (void)parent;

  CXCursor range_expr = clang_getNullCursor();
  clang_visitChildren( range_stmt_csr, &get_Expr_visitor, &range_expr );
  if ( clang_Cursor_isNull( range_expr ) )
    return;

  CXType type = clang_getCursorType( range_expr );
  bool is_const = clang_isConstQualifiedType( type );

  type = clang_getCanonicalType( type );
  type = clang_getNonReferenceType( type );
  if ( !is_const )
    is_const = clang_isConstQualifiedType( type );

  CXCursor const class_csr = clang_getTypeDeclaration( type );
  if ( tidy_Cursor_isClassDecl( class_csr ) ) {
    svd_cxx_for_range const prev_svd_cxx_for_range = svd->cxx_for_range;
    svd->cxx_for_range = (svd_cxx_for_range){ .is_const = is_const };
    clang_visitChildren( class_csr, &add_begin_end_ref_visitor, svd );
    svd->cxx_for_range = prev_svd_cxx_for_range;
  }
}

/**
 * Visits a `CXCursor_CXXMethod` kind of cursor.
 *
 * @remarks When a C++ virtual member function is called, libclang generates an
 * AST containing only the base class's member function since it can't know
 * which overridden functions are actually called.  So as not to them as
 * unused, add references from the base one to all the overridden ones.
 *
 * @param method_csr The method's cursor to visit.
 * @param parent The parent cursor of \a method_csr.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXMethod( CXCursor method_csr, CXCursor parent,
                             symbols_visit_data *svd ) {
  (void)parent;

  CXCursor *overridden_csrs;
  unsigned  overridden_csrs_count;
  clang_getOverriddenCursors(
    method_csr, &overridden_csrs, &overridden_csrs_count
  );

  for ( unsigned i = 0; i < overridden_csrs_count; ++i ) {
    tidy_symbol *const base_sym = add_symbol( overridden_csrs[i], svd->source );
    if ( base_sym != NULL )
      symbol_add_ref( base_sym, svd->parent_sym );
  } // for

  clang_disposeOverriddenCursors( overridden_csrs );
}

/**
 * Visits a `CXCursor_FieldDecl` kind of cursor.
 *
 * @remarks
 * @parblock
 * Ideally, we'd call visit_most_kinds() for a FieldDecl.  The problem is that,
 * given a field declaration like:
 *
 *      #define MAX_ALIGN         8
 *
 *      struct rb_node {
 *        // ...
 *        alignas( MAX_ALIGN ) char data[];
 *      };
 *
 * libclang's AST does _not_ include the `alignas` part, so we can't mark that
 * `MAX_ALIGN` is used.
 *
 * Therefore, we have to fall back to iterating over all tokens of the field's
 * declaration looking for identifiers to mark them as used.
 * @endparblock
 *
 * @param field_csr The field declaration's cursor to visit.
 * @param parent The parent cursor of \a field_csr.
 * @param svd The symbols_visit_data to use.
 */
static void visit_FieldDecl( CXCursor field_csr, CXCursor parent,
                             symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  CXSourceRange const field_range = tidy_getCursorExtent( field_csr );
  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( field_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tu, field_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor const    cls_csr = clang_getCursorSemanticParent( field_csr );
  CXString const    field_name_cxs = clang_getCursorSpelling( field_csr );
  char const *const field_name_cs  = clang_getCString( field_name_cxs );

  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;
    if ( tidy_Token_isEqualTo( tu, tokens[i], field_name_cs ) )
      continue;

    CXCursor const sym_csr = tidy_Token_getScopedNameCursor(
      tu, tokens, token_count, &i, cls_csr, svd->source->lang
    );
    tidy_symbol *const sym = add_symbol( sym_csr, svd->source );
    if ( sym != NULL )
      symbol_add_ref( svd->parent_sym, sym );
  } // for

  clang_disposeTokens( tu, tokens, token_count );
}

/**
 * Visits a `CXCursor_MacroDefinition` kind of cursor.
 *
 * @remarks
 * @parblock
 * We have to iterate over all tokens of the macro's definition looking for
 * identifiers to mark them as used.  For example, if a header contains:
 *
 *      #define POINTER_CAST(T,EXPR)    ((T)(uintptr_t)(EXPR))
 *
 * it should also `#include <stdint.h>` because `uintptr_t` is used.  The user
 * of the macro shouldn't have to know or care about the declaration, nor be
 * forced to `#include <stdint.h>` explicitly.
 * @endparblock
 *
 * @param macro_csr The macro definition's cursor to visit.
 * @param parent The parent cursor of \a macro_csr.
 * @param svd The symbols_visit_data to use.
 */
static void visit_MacroDefinition( CXCursor macro_csr, CXCursor parent,
                                   symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  CXSourceRange const macro_range = clang_getCursorExtent( macro_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( svd->source->tu, macro_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  //
  // While iterating over all tokens of the macro, we have to skip identifers
  // of macro parameters for function-like macros because those are obviously
  // defined by the macro itself.  To skip them, we first have to collect the
  // set of them.
  //
  hash_table_t param_set;
  ht_table_init(
    &param_set, HT_DINT, 2.0, 10,
    POINTER_CAST( ht_cmp_fn_t, &strcmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

  unsigned i = clang_Cursor_isMacroFunctionLike( macro_csr ) ?
    macro_get_params( svd->source->tu, tokens, token_count, &param_set ) :
    1;                                  // tokens[0] = macro name; start at 1

  for ( ; i < token_count; ++i ) {
    CXCursor const sym_csr = macro_Token_getScopedNameCursor(
      svd->source->tu, tokens, token_count, &i, &param_set, svd->source->lang
    );
    if ( !tidy_Cursor_isInvalid( sym_csr ) ) {
      tidy_symbol *const sym = add_symbol( sym_csr, svd->source );
      if ( sym != NULL )
        symbol_add_ref( svd->parent_sym, sym );
    }
  } // for

  ht_table_cleanup( &param_set, /*free_fn=*/NULL );
  clang_disposeTokens( svd->source->tu, tokens, token_count );
}

/**
 * Visits most kinds of reference cursor.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param svd The symbols_visitor_data to use.
 */
static void visit_most_Ref( CXCursor cursor, CXCursor parent,
                            symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  CXCursor const ref_csr = clang_getCursorReferenced( cursor );
  if ( unlikely( tidy_Cursor_isInvalid( ref_csr ) ) )
    return;

  tidy_symbol *const ref_sym = add_symbol( ref_csr, svd->source );
  if ( ref_sym != NULL ) {
    if ( svd->parent_sym != NULL )
      symbol_add_ref( svd->parent_sym, ref_sym );
    else
      ref_sym->is_root = true;
  }

  CXCursor const sem_parent = clang_getCursorSemanticParent( ref_csr );
  enum CXCursorKind const sem_parent_kind = clang_getCursorKind( sem_parent );
  if ( sem_parent_kind == CXCursor_EnumDecl ) {
    //
    // Special case: if any enumeration constant is referenced (which means its
    // semantic parent is an EnumDecl), mark the enumeration as a whole as
    // used.
    //
    tidy_symbol *const parent_sym = add_symbol( sem_parent, svd->source );
    if ( parent_sym != NULL )
      parent_sym->is_used = true;
  }
}

/**
 * Visits a `CXCursor_OverloadedDeclRef` kind of cursor.
 *
 * @remarks We have to iterate over all overloaded functions since calling
 * clang_getCursorReferenced() on a CXCursor_OverloadedDeclRef returns an
 * invalid or null cursor.
 *
 * @param overloaded_csr The overloaded definition's cursor to visit.
 * @param parent Not used.
 * @param svd The symbols_visitor_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr, CXCursor parent,
                                     symbols_visit_data *svd ) {
  (void)parent;

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    tidy_symbol *const dec_sym = add_symbol( dec_csr, svd->source );
    if ( dec_sym != NULL ) {
      if ( svd->parent_sym != NULL )
        symbol_add_ref( svd->parent_sym, dec_sym );
      else
        dec_sym->is_root = true;
    }
  } // for
}

/**
 * Visits all preprocessor directives that can reference symbols to mark them
 * as used.
 *
 * @remarks Libclang's AST doesn't include conditional preprocessor directives,
 * e.g., `#if`, `#ifdef`, etc.), so `clang_visitChildren()` can't visit them to
 * look for symbols that are referenced to mark them as used.  Consequently,
 * we're forced to iterate through all of \a source's tokens looking for `#`
 * followed by a directive name and check subsequent tokens for identifiers.
 *
 * @param source The source file whose preprocessor directives to visit.
 */
static void visit_preprocessor_directives( tidy_source const *source ) {
  assert( source != NULL );

  CXCursor const tu_csr = clang_getTranslationUnitCursor( source->tu );
  CXSourceRange const tu_range = clang_getCursorExtent( tu_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( source->tu, tu_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  for ( unsigned i = 0; i < token_count; ++i ) {
    CXSourceLocation const token_loc =
      clang_getTokenLocation( source->tu, tokens[i] );
    if ( clang_Location_isInSystemHeader( token_loc ) )
      continue;

    CXTokenKind token_kind = clang_getTokenKind( tokens[i] );
    if ( token_kind != CXToken_Punctuation )
      continue;
    if ( !tidy_Token_isEqualTo( source->tu, tokens[i], "#" ) )
      continue;
    if ( ++i == token_count )
      break;

    token_kind = clang_getTokenKind( tokens[i] );
    if ( token_kind != CXToken_Identifier )
      continue;
    int const directive_idx = tidy_Token_isEqualToAny( source->tu, tokens[i],
      "if", "ifdef", "ifndef", "elif", "elifdef", "elifndef"
    );
    if ( directive_idx == -1 )
      continue;

    unsigned last_line = tidy_getSpellingLocation_Line( token_loc );

    while ( ++i < token_count ) {
      unsigned const token_line =
        tidy_getTokenLocation_Line( source->tu, tokens[i] );
      if ( token_line > last_line ) {
        if ( !is_escaped_newline( source, tokens, i ) ) {
          --i;
          break;
        }
        last_line = token_line;
      }

      token_kind = clang_getTokenKind( tokens[i] );
      if ( token_kind != CXToken_Identifier )
        continue;
      CXString const token_cxs =
        clang_getTokenSpelling( source->tu, tokens[i] );
      char const *const token_cs = clang_getCString( token_cxs );

      if ( strcmp( token_cs, "defined" ) != 0 &&
           strcmp( token_cs, "__has_include" ) != 0 ) {
        CXCursor const sym_csr = tidy_getCursorByName( token_cs, tu_csr );
        tidy_symbol *const sym = add_symbol( sym_csr, source );
        if ( sym != NULL )
          sym->is_used = true;
      }

      clang_disposeString( token_cxs );
    } // while
  } // for

  clang_disposeTokens( source->tu, tokens, token_count );
}

////////// extern functions ///////////////////////////////////////////////////

int symbol_cmp( tidy_symbol const *i_sym, tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( j_sym != NULL );
  return strcmp( i_sym->key, j_sym->key );
}

ht_hash_val_t symbol_hash( tidy_symbol const *sym ) {
  return fnv1a_s( sym->key );
}

void symbols_init( void ) {
  ASSERT_RUN_ONCE();
  ht_table_init(
    &symbol_set, HT_DINT, 2.0, 1024,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &symbol_hash )
  );
  ATEXIT( &symbols_cleanup );
}

unsigned symbols_print_unused( void ) {
  unsigned const unused = symbols_mark_used();
  if ( unused == 0 )
    return 0;

  array_t symbol_array = ARRAY_INIT( sizeof(tidy_symbol) );
  array_reserve( &symbol_array, unused );

  ht_iterator_t iter;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( !(sym->path == NULL || sym->line == 0 || sym->is_used) )
      *(tidy_symbol*)array_push_back( &symbol_array ) = *sym;
  } // for

  array_qsort(
    &symbol_array,
    POINTER_CAST( array_cmp_fn_t, &symbol_cmp_by_path )
  );

  char const *prev_path = "";
  for ( size_t i = 0; i < symbol_array.len; ++i ) {
    tidy_symbol const *const sym = array_at_nc( &symbol_array, i );
    if ( strcmp( sym->path, prev_path ) != 0 ) {
      color_start( stderr, sgr_locus );
      EPRINTF( "\"%s\"", path_no_dot_slash( sym->path ) );
      color_end( stderr, sgr_locus );
      EPUTS( ": " );
      prev_path = sym->path;

      color_start( stderr, sgr_warning );
      EPUTS( "warning" );
      color_end( stderr, sgr_warning );
      EPUTS( ": unused symbols:\n" );
    }

    color_start( stderr, sgr_locus );
    EPRINTF( "%5u", sym->line );
    color_end( stderr, sgr_locus );
    EPUTC( ',' );
    color_start( stderr, sgr_locus );
    EPRINTF( "%2u", sym->col );
    color_end( stderr, sgr_locus );
    EPRINTF( " | \"%s\"\n", sym->display_name ? sym->display_name : sym->name );
  } // for

  array_cleanup( &symbol_array, /*free_fn=*/NULL );

  EPRINTF(
    "%s: %u unused symbol%s\n",
    prog_name, unused, plural_s( unused )
  );
  return unused;
}

void symbols_visit( tidy_source *source ) {
  symbols_visit_data svd = {
    .source = source,
    .cxx_constructor = { .ctor_csr = clang_getNullCursor() }
  };
  CXCursor const cursor = clang_getTranslationUnitCursor( source->tu );
  clang_visitChildren( cursor, &symbols_visitor, &svd );
  visit_preprocessor_directives( source );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
