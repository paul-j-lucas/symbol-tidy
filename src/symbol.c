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
#include "clang_util.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "options.h"
#include "print.h"
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
#include <stdlib.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup tidy-symbols-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
typedef struct symbols_visit_data symbols_visit_data;
/// @endcond

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data passed to symbols_visitor().
 */
struct symbols_visit_data {
  /**
   * The source file being tidied.
   */
  tidy_source const *source;

  /**
   * TODO.
   */
  tidy_symbol *parent_sym;
};

////////// local functions ////////////////////////////////////////////////////

static void symbol_cleanup( tidy_symbol* );

NODISCARD
static bool symbol_is_excluded( CXCursor, symbols_visit_data const* );

static void visit_FieldDecl( CXCursor, CXCursor, symbols_visit_data* );
static void visit_MacroDefinition( CXCursor, CXCursor, symbols_visit_data* );
static void visit_MemberRefExpr( CXCursor, CXCursor, symbols_visit_data* );
static void visit_most_kinds( CXCursor, CXCursor, symbols_visit_data* );
static void visit_OverloadedDeclRef( CXCursor, CXCursor, symbols_visit_data* );

////////// local variables ////////////////////////////////////////////////////

static hash_table_t symbol_set;         ///< Set of symbols.

////////// local functions ////////////////////////////////////////////////////

/**
 * Adds a symbol to the global set and marks the header that declares is as
 * necessary.
 *
 * @param sym_csr The cursor for the symbol to add.
 * @param sym_file The file that contains the declaration for \a sym_csr.
 * @param svd The symbols_visit_data to use.
 *
 * @sa maybe_add_symbol()
 */
static tidy_symbol* add_symbol( CXCursor sym_csr ) {
  char *key = tidy_getCanonicalCursorUSR( sym_csr );
  if ( key == NULL )
    return NULL;

  tidy_symbol *sym = NULL;

  CXString const name_cxs = clang_getCursorSpelling( sym_csr );
  char const *const name_cs = clang_getCString( name_cxs );
  if ( name_cs == NULL || name_cs[0] == '\0' )
    goto done;
  if ( strcmp( name_cs, TIDY_COMPILER_ID ) == 0 )
    goto done;

  tidy_symbol new_sym = { .key = key };
  key = NULL;                           // new_sym owns this now
  ht_insert_rv_t const hti =
    ht_table_insert( &symbol_set, &new_sym, sizeof new_sym );
  sym = HT_DINT( hti.entry );

  if ( !hti.inserted ) {
    symbol_cleanup( &new_sym );
    goto done;
  }

  sym->name = strdup_or_exit( name_cs );
  if ( tidy_Cursor_isExternal( sym_csr ) && strcmp( sym->name, "main" ) == 0 )
    sym->is_root = true;

done:
  free( key );
  clang_disposeString( name_cxs );
  return sym;
}

/**
 * Gets the file containing \a sym_csr unless it's the file being tidied in
 * which case we don't want to add that symbol to the global set.
 *
 * @param sym_csr The cursor for the symbol.
 * @param svd The symbols_visit_data to use.
 * @return Returns the file containing \a sym_csr or NULL.
 */
NODISCARD
static CXFile get_symbol_file( CXCursor sym_csr,
                               symbols_visit_data const *svd ) {
  assert( svd != NULL );

  CXFile const sym_file = tidy_getCursorLocation_File( sym_csr );
  if ( unlikely( sym_file == NULL ) )
    return NULL;                        // LCOV_EXCL_LINE

  // If the symbol was first declared in the file being tidied, we don't care.
  if ( clang_File_isEqual( sym_file, svd->source->file ) )
    return NULL;

  return sym_file;
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
 * Maybe adds a symbol to the global set and marks the header that declares is
 * as necessary.
 *
 * @remarks This is a convenience function for the common case of calling
 * symbol_is_excluded(), get_symbol_file(), and add_symbol().
 *
 * @param name_csr The cursor to use for the name of the symbol.  It may be
 * (and often is) the same as \a sym_csr.
 * @param sym_csr The cursor for the symbol to add.
 * @param svd The symbols_visit_data to use.
 */
static void maybe_add_symbol( CXCursor sym_csr, symbols_visit_data *svd ) {
  if ( symbol_is_excluded( sym_csr, svd ) )
    return;
  add_symbol( sym_csr );
}

/**
 * Prints statistics for symbols if requested.
 */
static void print_statistics( void ) {
  if ( !verbose_print_statistics() )
    return;

  verbose_printf( "  symbol set:\n" );
  verbose_printf(
    "    ss-load-factor = " TIDY_STAT_LF_FMT "\n",
    ht_table_load_factor( &symbol_set )
  );
  verbose_printf( "    ss-size = %u\n", symbol_set.size );
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

  tidy_sym_ref *ref = from_sym->refs;
  while ( ref != NULL ) {
    if ( ref->sym == to_sym )
      return;
    ref = ref->next;
  } // while

  ref = MALLOC( tidy_sym_ref, 1 );
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
    FREE( sym->file );
    FREE( sym->key );
    FREE( sym->name );

    for ( tidy_sym_ref *ref = sym->refs, *next; ref != NULL; ref = next ) {
      next = ref->next;
      free( ref );
    } // for
  }
}

/**
 * Gets whether \a sym_csr should be excluded from the global set.
 *
 * @param sym_csr The symbol's cursor to check.
 * @param svd The symbols_visit_data to use.
 * @return Returns `true` only if \a sym_csr is excluded.
 */
NODISCARD
static bool symbol_is_excluded( CXCursor sym_csr,
                                symbols_visit_data const *svd ) {
  if ( tidy_Cursor_isInvalid( sym_csr ) )
    return true;

  assert( svd != NULL );

  //
  // We can't use clang_getCursorLanguage() because libclang doesn't initially
  // set what it thinks the current language is until it encounters something
  // language-specific.  So just use svd->source->lang.
  //
  if ( tidy_Cursor_isReservedName( sym_csr, svd->source->lang ) )
    return true;

  enum CXCursorKind const sym_kind = clang_getCursorKind( sym_csr );
  switch ( sym_kind ) {
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_ConversionFunction:
    case CXCursor_Destructor:
      //
      // Even though the switch in symbols_init_visitor() doesn't include cases
      // for all of these, the referenced cursor obtained in visit_most_kinds()
      // may turn out to be one of these.
      //
      // However, adding the symbol for one of these would trigger a false-
      // positive include dependency for merely _calling_ the symbol when
      // inherited, e.g.:
      //
      //      // Base.hpp
      //      struct Base {
      //        void f();
      //      };
      //
      //      // Derived.hpp
      //      #include "Base.hpp"
      //      struct Derived : Base {
      //        void g();
      //      };
      //
      //      // Derived.cpp
      //      #include "Derived.hpp"
      //      void Derived::g() {
      //        f();
      //      }
      //
      // If these cases weren't skipped, then the call of f() in Derived.cpp
      // would trigger a dependency on Base.hpp because that's where f() is
      // declared.
      //
      // However, since Derived is derived from Base, that means the definition
      // of Base was available via Derived.hpp including Base.hpp and that's
      // sufficient --- an exception to IWYU.
      //
      return true;

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
 * TODO
 *
 * @param sym The symbol to start at.
 */
static void symbol_mark_reachable( tidy_symbol *sym ) {
  assert( sym != NULL );

  if ( !sym->is_reachable ) {
    sym->is_reachable = true;
    for ( tidy_sym_ref *ref = sym->refs; ref != NULL; ref = ref->next )
      symbol_mark_reachable( ref->sym );
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

  CXSourceLocation const loc = clang_getCursorLocation( cursor );
  if ( clang_Location_isInSystemHeader( loc ) )
    goto skip;

  if ( IS_VERBOSE( CURSORS ) )
    verbose_print_cursor( cursor );     // LCOV_EXCL_LINE

  assert( data != NULL );
  symbols_visit_data *const svd = data;

  bool is_definition = false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );

  switch ( kind ) {
    case CXCursor_FunctionDecl:
    case CXCursor_VarDecl:
      if ( !clang_isCursorDefinition( cursor ) )
        break;
      if ( tidy_Cursor_isExternal( cursor ) )
        is_definition = true;
      break;

    case CXCursor_MacroDefinition:
      is_definition = true;
      break;

    default:
      /* suppress warning */;
  } // switch

  tidy_symbol *sym = NULL;

  if ( is_definition ) {
    sym = add_symbol( cursor );
    if ( sym != NULL ) {
      /* Update file/line to point to the actual definition */
      CXFile file;
      clang_getSpellingLocation( loc, &file, &sym->line, &sym->col, NULL );
      CXString const file_cxs = clang_getFileName( file );
      char const *const file_cs = clang_getCString( file_cxs );
      if ( file_cs != NULL && file_cs[0] != '\0' ) {
        FREE( sym->file );              // just in case
        sym->file = strdup_or_exit( file_cs );
      }
      clang_disposeString( file_cxs );

      if ( kind == CXCursor_FunctionDecl )
        svd->parent_sym = sym;
    }
  }

  /* Record usages/references using canonical target cursors */
  switch ( kind ) {
    case CXCursor_CallExpr:
    case CXCursor_DeclRefExpr:
    case CXCursor_MacroExpansion:
    case CXCursor_MemberRefExpr:;
      CXCursor const ref_csr = clang_getCursorReferenced( cursor );
      if ( !tidy_Cursor_isInvalid( ref_csr ) ) {
        tidy_symbol *const ref_sym = add_symbol( ref_csr );
        if ( ref_sym != NULL ) {
          if ( svd->parent_sym != NULL )
            symbol_add_ref( svd->parent_sym, ref_sym );
          else
            ref_sym->is_root = true;
        }
      }
      break;
    default:
      /* suppress warning */;
  } // switch

  if ( sym != NULL ) {
    switch ( kind ) {
      case CXCursor_MacroDefinition:
        sym->is_root = true;
        svd->parent_sym = sym;
        visit_MacroDefinition( cursor, parent, svd );
        break;
      default:
        /* suppress warning */;
    } // switch
  }

  clang_visitChildren( cursor, symbols_visitor, svd );
  svd->parent_sym = NULL;

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
 * Visits a `CXCursor_FieldDecl` kind of cursor.
 *
 * @remarks
 * @parblock
 * Ideally, we'd call visit_most_kinds() for a FieldDecl.  The problem is that,
 * given a field declaration like:
 *
 *      struct rb_node {
 *        // ...
 *        alignas( max_align_t ) char data[];
 *      };
 *
 * libclang's AST does _not_ include the `alignas` part, so we can't check that
 * the header that declares either `alignas` (for C &lt; C23) or `max_align_t`
 * has been included.
 *
 * Therefore, we have to fall back to iterating over all tokens of the field's
 * declaration looking for identifiers to see whether the header that declares
 * them has been included.
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

  CXString const    field_name_cxs = clang_getCursorSpelling( field_csr );
  char const *const field_name_cs  = clang_getCString( field_name_cxs );

  CXCursor const cls_csr = clang_getCursorSemanticParent( field_csr );

  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;

    CXString const    token_cxs = clang_getTokenSpelling( tu, tokens[i] );
    char const *const token_cs  = clang_getCString( token_cxs );
    bool const        is_field_name = strcmp( token_cs, field_name_cs ) == 0;

    clang_disposeString( token_cxs );

    if ( is_field_name )
      continue;

    CXCursor const sym_csr = tidy_Token_getScopedNameCursor(
      svd->source->tu, tokens, token_count, &i, cls_csr, svd->source->lang
    );
    maybe_add_symbol( sym_csr, svd );
  } // for

  clang_disposeTokens( tu, tokens, token_count );
}

/**
 * Visits a `CXCursor_MacroDefinition` kind of cursor.
 *
 * @remarks
 * @parblock
 * We have to iterate over all tokens of the macro's definition looking for
 * identifiers to mark them as reachable.
 * For example, if a header contains:
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
  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( macro_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tu, macro_range, &tokens, &token_count );
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
    maybe_add_symbol( sym_csr, svd );
  } // for

  ht_table_cleanup( &param_set, /*free_fn=*/NULL );
  clang_disposeTokens( tu, tokens, token_count );
}

/**
 * Visit most kinds of cursor.
 *
 * @param cursor The cursor to visit.
 * @param parent The parent cursor of \a cursor.
 * @param svd The symbols_visit_data to use.
 */
static void visit_most_kinds( CXCursor cursor, CXCursor parent,
                              symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  // Gets the cursor for the declaration of the symbol.
  CXCursor const dec_csr = clang_getCursorReferenced( cursor );
  if ( tidy_Cursor_isInvalid( dec_csr ) )
    return;                             // LCOV_EXCL_LINE

  //
  // Call symbol_is_excluded() and get_symbol_file() ourselves to avoid calling
  // is_cxx_proxy_qual_ref_iwyu_exc() and is_cxx_mbr_or_base_iwyu_exc() unless
  // necessary since they're both expensive.
  //
  if ( !symbol_is_excluded( dec_csr, svd ) ) {
    CXFile const dec_file = get_symbol_file( dec_csr, svd );
    if ( dec_file != NULL ) {
      add_symbol( dec_csr );
    }
  }
}

/**
 * Visits a `CXCursor_MemberRefExpr` kind of cursor.
 *
 * @param mbr_ref_csr The member reference's cursor to visit.
 * @param parent Not used.
 * @param svd The symbols_visit_data to use.
 */
static void visit_MemberRefExpr( CXCursor mbr_ref_csr, CXCursor parent,
                                 symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  CXCursor const mbr_csr = clang_getCursorReferenced( mbr_ref_csr );
  if ( tidy_Cursor_isInvalid( mbr_csr ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor const mbr_cls_csr = clang_getCursorSemanticParent( mbr_csr );
  visit_most_kinds( mbr_ref_csr, mbr_cls_csr, svd );
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
 * @param svd The symbols_visit_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr, CXCursor parent,
                                     symbols_visit_data *svd ) {
  (void)parent;
  assert( svd != NULL );

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    maybe_add_symbol( dec_csr, svd );
    //
    // It's possible that different overloads will be declared in different
    // headers.  But for now, we stop after the first overload.
    //
    break;
  } // for
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

bool is_input_file( char const *file, int argc, char const **argv) {
  if ( file == NULL )
    return false;
  for ( int i = 1; i < argc; ++i ) {
    if ( strcmp( file, argv[i] ) == 0 )
      return true;
  } // for
  return false;
}

unsigned symbols_report_unused( void ) {
  ht_iterator_t iter;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_root )
      symbol_mark_reachable( sym );
  } // for

  unsigned unused = 0;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_reachable )
      continue;
    print_file_warning( sym->file, sym->line, sym->col,
      "\"%s\": unused symbol", sym->name
    );
    ++unused;
  } // for

  if ( unused > 0 )
    EPRINTF( "%s: %u unused symbols\n", prog_name, unused );
  return unused;
}

void symbols_visit( tidy_source *source ) {
  symbols_visit_data svd = { .source = source };
  CXCursor const cursor = clang_getTranslationUnitCursor( source->tu );
  clang_visitChildren( cursor, &symbols_visitor, &svd );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
