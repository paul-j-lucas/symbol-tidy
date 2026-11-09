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
#include "cxx_symbol.h"
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

typedef struct tidy_header        tidy_header;
typedef struct tidy_header_stats  tidy_header_stats;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A header included in a translation unit.
 */
struct tidy_header {
  CXFileUniqueID      fuid;             ///< The file's unique ID.
  enum CXLanguageKind lang;             ///< From tidy_source.
  unsigned            tsid;             ///< From tidy_source.
};

/**
 * Additional statistics for the set of headers that were included across all
 * translation units.
 */
struct tidy_header_stats {
  unsigned  revisit_count;              ///< Headers visited more than once.
  unsigned  skipped_count;              ///< Headers skipped.
};

////////// local functions ////////////////////////////////////////////////////

NODISCARD
static int  symbol_cmp( tidy_symbol const*, tidy_symbol const* );

static void symbol_cleanup( tidy_symbol* );

static void visit_Constructor( CXCursor, symbols_visit_data* );
static void visit_CXXDeleteExpr( CXCursor, symbols_visit_data* );
static void visit_CXXForRangeStmt( CXCursor, symbols_visit_data* );
static void visit_CXXMethod( CXCursor, symbols_visit_data* );
static void visit_CXXNewExpr( CXCursor, symbols_visit_data* );
static void visit_FieldDecl( CXCursor, symbols_visit_data* );
static void visit_FunctionDecl( CXCursor, symbols_visit_data* );
static void visit_MacroDefinition( CXCursor, symbols_visit_data* );
static void visit_most_Ref( CXCursor, symbols_visit_data* );
static void visit_OverloadedDeclRef( CXCursor, bool, symbols_visit_data* );
static void visit_VarDecl( CXCursor, symbols_visit_data* );

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

hash_table_t tidy_symbol_set;

/// @endcond

////////// local variables ////////////////////////////////////////////////////

static hash_table_t       header_set;   ///< The set of all header files.
static tidy_header_stats  header_stats; ///< Statistics for \ref header_set.

////////// inline functions ///////////////////////////////////////////////////

/**
 * Gets the name of \a sym that should be used for printing.
 *
 * @remarks For C, it's just the symbol's plain name; for C++, it's the
 * symbol's prototype so overloaded functions can be distinguished.
 *
 * @param sym The tidy_symbol to get the name of.
 * @return Returns said name.
 */
NODISCARD
static inline char const* symbol_name_for_print( tidy_symbol const *sym ) {
  return sym->cxx_prototype ? sym->cxx_prototype : sym->name;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Gets whether \a cursor is nested, at any depth, within a class (or
 * enumeration) that is ignored via the `ignore-symbols` key.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is within an ignored class (or
 * enumeration).
 */
NODISCARD
static bool cursor_in_ignored_class( CXCursor cursor ) {
  CXCursor scope_csr = clang_getCursorSemanticParent( cursor );

  while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
    enum CXCursorKind const kind = clang_getCursorKind( scope_csr );
    if ( kind == CXCursor_TranslationUnit )
      break;

    if ( tidy_Cursor_isClassDecl( scope_csr ) || kind == CXCursor_EnumDecl ) {
      CXFile const  scope_file = tidy_getCursorLocation_File( scope_csr );
      char *const   scope_name = tidy_Cursor_getScopedSpelling( scope_csr );
      bool          is_ignored = false;

      if ( scope_file != NULL && scope_name != NULL && scope_name[0] != '\0' ) {
        CXString const    path_cxs = clang_getFileName( scope_file );
        char const *const path_cs = clang_getCString( path_cxs );
        is_ignored = config_symbol_is_ignored( scope_name, path_cs );
        clang_disposeString( path_cxs );
      }
      free( scope_name );

      if ( is_ignored )
        return true;
    }

    scope_csr = clang_getCursorSemanticParent( scope_csr );
  } // while

  return false;
}

/**
 * Gets whether \a sym_csr should be excluded from the global set.
 *
 * @param sym_csr The symbol's cursor to check.
 * @param source The tidy_source to use.
 * @return Returns `true` only if \a sym_csr is excluded.
 */
NODISCARD
static bool cursor_is_excluded( CXCursor sym_csr, tidy_source const *source ) {
  if ( tidy_Cursor_isInvalid( sym_csr ) )
    return true;

  CXFile const sym_file = tidy_getCursorLocation_File( sym_csr );
  if ( sym_file == NULL )
    return true;

  assert( source != NULL );

  if ( tidy_Cursor_inAnonymousClass( sym_csr ) )
    return true;

  //
  // We can't use clang_getCursorLanguage() because libclang doesn't initially
  // set what it thinks the current language is until it encounters something
  // language-specific.  So just use source->lang.
  //
  if ( tidy_Cursor_isReservedName( sym_csr, source->lang ) )
    return true;

  char *cls_name = NULL;
  bool  is_excluded = true;
  char *sym_scoped_name = tidy_Cursor_getScopedSpelling( sym_csr );

  if ( sym_scoped_name == NULL )
    goto done;
  if ( strcmp( sym_scoped_name, TIDY_COMPILER_ID ) == 0 )
    goto done;

  if ( cursor_in_ignored_class( sym_csr ) )
    goto done;

  is_excluded = false;

  CXString const    sym_path_cxs = clang_getFileName( sym_file );
  char const *const sym_path_cs = clang_getCString( sym_path_cxs );
  bool const ignored = config_symbol_is_ignored( sym_scoped_name, sym_path_cs );
  clang_disposeString( sym_path_cxs );

  CXCursor const          parent_csr = clang_getCursorSemanticParent( sym_csr );
  enum CXCursorKind const sym_kind = clang_getCursorKind( sym_csr );

  switch ( sym_kind ) {
    case CXCursor_Constructor:
    case CXCursor_CXXMethod:
      // Get the (expensive) class name only when needed.
      cls_name = tidy_Cursor_getScopedSpelling( parent_csr );
      break;
    default:
      /* suppress warning */;
  } // switch

  switch ( sym_kind ) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_EnumConstantDecl:
    case CXCursor_EnumDecl:
    case CXCursor_FieldDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_MacroDefinition:
    case CXCursor_StructDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_UnionDecl:
    case CXCursor_VarDecl:
      is_excluded = ignored;
      break;

    case CXCursor_Constructor:
      if ( clang_CXXConstructor_isCopyConstructor( sym_csr ) &&
           config_bool_value( &opt_ignore_cxx_copy_ctors, cls_name ) ) {
        is_excluded = true;
      }
      if ( clang_CXXConstructor_isMoveConstructor( sym_csr ) &&
           config_bool_value( &opt_ignore_cxx_move_ctors, cls_name ) ) {
        is_excluded = true;
      }
      break;

    case CXCursor_CXXMethod:
      if ( ignored ) {
        is_excluded = true;
      }
      else if ( clang_CXXMethod_isCopyAssignmentOperator( sym_csr ) ) {
        if ( config_bool_value( &opt_ignore_cxx_copy_assigns, cls_name ) )
          is_excluded = true;
      }
      else if ( clang_CXXMethod_isMoveAssignmentOperator( sym_csr ) ) {
        if ( config_bool_value( &opt_ignore_cxx_move_assigns, cls_name ) )
          is_excluded = true;
      }
      break;

    case CXCursor_Namespace:
      //
      // Namespaces first declared in standard headers (e.g., "std") should be
      // excluded even if re-opened in user code.
      //
      sym_csr = clang_getCanonicalCursor( sym_csr );
      return tidy_Cursor_isInSystemHeader( sym_csr );

    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_TemplateTemplateParameter:
    case CXCursor_TemplateTypeParameter:
      //
      // These are local to the template definition, so don't need to be added
      // to tidy_symbol_set.
      //
      return true;

    case CXCursor_ParmDecl:
      //
      // These are local to a function, so don't need to be added to
      // tidy_symbol_set.
      //
      return true;

    default:
      /* suppress warning */;
  } // switch

done:
  free( cls_name );
  free( sym_scoped_name );
  return is_excluded;
}

/**
 * Gets whether the file for a top-level cursor can be skipped because it was
 * already visted for an earlier translation unit of the same language.
 *
 * @remarks The first top-level cursor from a file registers it with the
 * current TU's serial and is visted, as are the rest of that file's cursors in
 * the same TU.  In later TUs, the serial differs, so they're skipped.
 *
 * @param file The cursor's file.
 * @param svd The symbols_visit_data to use.
 * @return Returns `true` only if the file was already seen.
 */
NODISCARD
static bool file_previously_seen( CXFile file, symbols_visit_data *svd ) {
  assert( svd != NULL );

  if ( file == NULL || file == svd->source->file )
    return false;                       // never skip the TU's own source

  //
  // Consecutive top-level cursors are usually from the same file: if so,
  // return the last result.
  //
  if ( file == svd->fps.last_file )
    return svd->fps.last_seen;

  bool seen = false;

  tidy_header find_header = {
    .fuid = tidy_getFileUniqueID( file ),
    .lang = svd->source->lang,
    .tsid = svd->source->tsid
  };
  ht_insert_rv_t const hti =
    ht_table_insert( &header_set, &find_header, sizeof find_header );
  if ( !hti.inserted ) {
    tidy_header const *const found_header = HT_DINT( hti.entry );
    seen = found_header->tsid != svd->source->tsid;
    if ( seen )
      ++header_stats.skipped_count;
    else
      ++header_stats.revisit_count;
  }

  svd->fps.last_file = file;
  svd->fps.last_seen = seen;
  return seen;
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
 * Prints statistics for a hash table that has the maximum load factor among
 * all symbols.
 *
 * @param mbr_off The offset of the hash table (from `offsetof`).
 * @param abbr The abbreviation to use.
 */
static void print_max_lf_stats( size_t mbr_off, char const *abbr ) {
  tidy_symbol const *max_sym = NULL;
  double max_lf = -1.0;

  ht_iterator_t iter;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_dead )
      continue;
    hash_table_t const *const set = PTR_TO_MBR( sym, mbr_off );
    if ( set->size > 0 ) {
      double const lf = ht_table_load_factor( set );
      if ( lf > max_lf ) {
        max_sym = sym;
        max_lf = lf;
      }
    }
  } // for

  if ( max_sym != NULL ) {
    verbose_printf(
      "    %s-max-load-factor:\n", abbr
    );
    verbose_printf(
      "      %s-name = \"%s\"\n", abbr, symbol_name_for_print( max_sym )
    );
    if ( max_sym->path != NULL ) {
      verbose_printf(
        "      %s-path = \"%s\"\n", abbr, max_sym->path
      );
    }
    verbose_printf(
      "      %s-load-factor = " TIDY_STAT_LF_FMT "\n", abbr, max_lf
    );
    verbose_printf(
      "      %s-size = %u\n", abbr, max_sym->ref_sym_set.size
    );
  }
}

/**
 * Prints statistics for symbols if requested.
 */
static void print_statistics( void ) {
  verbose_printf(
    "  header set:\n"
  );
  verbose_printf(
    "    hs-load-factor = " TIDY_STAT_LF_FMT "\n",
                          ht_table_load_factor( &header_set )
  );
  verbose_printf(
    "    hs-size = %u\n", header_set.size
  );

  verbose_printf(
    "    hs-revisit-count = %u\n", header_stats.revisit_count
  );
  verbose_printf(
    "    hs-skipped-count = %'u\n", header_stats.skipped_count
  );

  verbose_printf(
    "  symbol set:\n"
  );
  verbose_printf(
    "    ss-load-factor = " TIDY_STAT_LF_FMT "\n",
                          ht_table_load_factor( &tidy_symbol_set )
  );
  verbose_printf(
    "    ss-size = %u\n", tidy_symbol_set.size
  );

  print_max_lf_stats( offsetof( tidy_symbol, ref_sym_set ), "ss-rss" );
  print_max_lf_stats( offsetof( tidy_symbol, cxx_adl_usr_set ), "ss-adlus" );
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
    FREE( sym->cxx_prototype );
    FREE( sym->path );
    ht_table_cleanup( &sym->cxx_adl_usr_set, /*free_fn=*/NULL );
    ht_table_cleanup( &sym->ref_sym_set, /*free_fn=*/NULL );
  }
}

/**
 * Compares two \ref tidy_symbol objects.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the name of \a
 * i_sym is less than, equal to, or greater than the name of \a j_sym,
 * respectively.
 *
 * @sa symbol_cmp_by_path()
 */
NODISCARD
static int symbol_cmp( tidy_symbol const *i_sym, tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( j_sym != NULL );
  return strcmp( i_sym->key, j_sym->key );
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
 *
 * @sa symbol_cmp()
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
 * Calculates the hash of \a sym.
 *
 * @param sym The tidy_symbol to calculate the hash for.
 * @return Returns said hash.
 */
NODISCARD
static ht_hash_val_t symbol_hash( tidy_symbol const *sym ) {
  return fnv1a_s( sym->key );
}

/**
 * Gets whether \a sym is within a scope that is ununsed.
 *
 * @param sym The symbol to check.
 * @return Returns `true` only if \a sym is within an unused scope.
 */
NODISCARD
static bool symbol_in_unused_scope( tidy_symbol const *sym ) {
  assert( sym != NULL );

  for ( sym = sym->parent_sym; sym != NULL; sym = sym->parent_sym ) {
    if ( sym->is_scope && !sym->is_used )
      return true;
  } // for

  return false;
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
    if ( sym->parent_sym != NULL )
      symbol_mark_used( sym->parent_sym );
    ht_iterator_t iter;
    ht_iterator_init( &iter, &sym->ref_sym_set );
    for ( tidy_symbol *ref_sym;
          (ref_sym = ht_iterator_next( &iter )) != NULL; ) {
      symbol_mark_used( ref_sym );
    } // for
  }
}

/**
 * Cleans-up all symbols.
 */
static void symbols_cleanup( void ) {
  verbose_print_statistics( &print_statistics );
  ht_table_cleanup( &header_set, /*free_fn=*/NULL );
  ht_table_cleanup(
    &tidy_symbol_set, POINTER_CAST( ht_free_fn_t, &symbol_cleanup )
  );
}

/**
 * Marks all \ref tidy_symbol::is_used "used" symbols as used and \ref
 * tidy_symbol::is_dead "dead" symbols as dead.
 *
 * @return Returns the number of unused, alive symbols.
 */
static unsigned symbols_mark_used( void ) {
  cxx_symbols_mark_adl_roots();

  ht_iterator_t iter;

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_root )
      symbol_mark_used( sym );
  } // for

  unsigned unused = 0;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->path == NULL || sym->line == 0 ||
         symbol_in_unused_scope( sym ) ) {
      sym->is_dead = true;
    }
    else if ( !sym->is_used ) {
      ++unused;
    }
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
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  CXSourceLocation const sym_loc = clang_getCursorLocation( cursor );
  bool const is_in_system_header = clang_Location_isInSystemHeader( sym_loc );

  if ( is_in_system_header ) {
    //
    // Symbols in standard headers can be skipped entirely in C.
    //
    if ( svd->source->lang == CXLanguage_C )
      goto skip;
    //
    // In C++, they can't be skipped because template code in standard headers
    // calls back into user code, so we need it to mark the user code as used.
    //
  }
  else if ( IS_VERBOSE( CURSORS ) ) {
    verbose_print_cursor( cursor );     // LCOV_EXCL_LINE
  }

  if ( is_in_system_header /* || opt_skip_user_headers*/ ) {
    enum CXCursorKind const parent_kind = clang_getCursorKind( parent );
    if ( parent_kind == CXCursor_TranslationUnit ) {
      //
      // For a top-level cursor, skip it (and its whole subtree) if its file
      // was already included by an earlier translation unit.
      //
      CXFile const sym_file = tidy_getFileLocation_File( sym_loc );
      if ( file_previously_seen( sym_file, svd ) )
        goto skip;
    }
  }

  tidy_symbol *const prev_sym = svd->curr_sym;

  bool is_definition = false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );

  if ( svd->source->lang == CXLanguage_CPlusPlus && !is_in_system_header &&
       (clang_isDeclaration( kind ) || clang_isExpression( kind )) ) {
    //
    // Remember the user-defined classes used as template arguments so we can
    // later tell which user functions ADL could find from standard templates.
    //
    CXType const type = clang_getCursorType( cursor );
    cxx_adl_type_set_add( type );
  }

  switch ( kind ) {
    case CXCursor_CallExpr:
    case CXCursor_CXXFunctionalCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_DeclRefExpr:
    case CXCursor_LabelRef:
    case CXCursor_MacroExpansion:
    case CXCursor_MemberRefExpr:
    case CXCursor_TemplateRef:
    case CXCursor_TypeRef:
      visit_most_Ref( cursor, svd );
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

    case CXCursor_CXXDeleteExpr:
      visit_CXXDeleteExpr( cursor, svd );
      break;

    case CXCursor_CXXForRangeStmt:
      visit_CXXForRangeStmt( cursor, svd );
      break;

    case CXCursor_CXXNewExpr:
      visit_CXXNewExpr( cursor, svd );
      break;

    case CXCursor_EnumConstantDecl:
    case CXCursor_FieldDecl:
    case CXCursor_LabelStmt:
    case CXCursor_MacroDefinition:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
      is_definition = true;
      break;

    case CXCursor_EnumDecl:
    case CXCursor_Namespace:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      if ( !clang_Cursor_isAnonymous( cursor ) )
        is_definition = true;
      break;

    case CXCursor_OverloadedDeclRef:
      visit_OverloadedDeclRef( cursor, is_in_system_header, svd );
      break;

    default:
      /* suppress warning */;
  } // switch

  if ( is_definition ) {
    tidy_symbol *const sym = symbol_add( cursor, svd->source );
    if ( likely( sym != NULL ) ) {
      if ( !is_in_system_header ) {
        //
        // For definitions, set the symbol's path, line, and column.
        //
        CXFile sym_file;
        clang_getSpellingLocation(
          sym_loc, &sym_file, &sym->line, &sym->col, /*offset=*/NULL
        );

        if ( sym_file != NULL ) {
          CXString const    path_cxs = clang_getFileName( sym_file );
          char const *const path_cs = clang_getCString( path_cxs );
          if ( is_source_path( path_cs ) ) {
            FREE( sym->path );          // just in case
            sym->path = strdup_or_exit( path_no_dot_slash( path_cs ) );
          }
          clang_disposeString( path_cxs );
        }
      }

      if ( sym->parent_sym == NULL )
        sym->parent_sym = svd->curr_sym;

      switch ( kind ) {
        case CXCursor_LabelStmt:
          //
          // Never set curr_sym to a labeled statement since it can break a
          // ref_sym_set.  Consider:
          //
          //      void g() { }
          //
          //      void f() {
          //        unused_label: g();
          //      }
          //
          //      int main() {
          //        f();
          //      }
          //
          // If we set curr_sym to unused_label here, we'd get the following
          // symbols and what they reference:
          //
          //      f() -> {}
          //      g() -> {}
          //      main() [root] -> { f() }
          //      unused_label -> { g() }
          //
          // Note that f() does _not_ reference unused_label.  (If it did, then
          // ununsed label would incorrectly be considered used.  A actual goto
          // statement is needed to reference it.)
          //
          // Since nothing references unused_label, it's correctly left as
          // unused.  But unused_label is the only symbol that has a reference
          // to g(), so g() would be left as unused also --- incorrectly.
          //
          // By _not_ setting curr_sym to unused_label, we'd get the following
          // symbols and what they reference:
          //
          //      f() -> { g() }
          //      g() -> {}
          //      main() [root] -> { f() }
          //      unused_label -> {}
          //
          // Now, f() references g(), so it would be marked used --- correctly.
          //
          break;

        default:
          svd->curr_sym = sym;
      } // switch

      if ( !is_in_system_header ) {
        switch ( kind ) {
          case CXCursor_Constructor:
            visit_Constructor( cursor, svd );
            break;

          case CXCursor_CXXMethod:
          case CXCursor_Destructor:
            visit_CXXMethod( cursor, svd );
            break;

          case CXCursor_FieldDecl:
            visit_FieldDecl( cursor, svd );
            break;

          case CXCursor_FunctionDecl:
            visit_FunctionDecl( cursor, svd );
            break;

          case CXCursor_MacroDefinition:
            visit_MacroDefinition( cursor, svd );
            break;

          case CXCursor_VarDecl:
            visit_VarDecl( cursor, svd );
            break;

          default:
            /* suppress warning */;
        } // switch
      }
    }
  }

  clang_visitChildren( cursor, symbols_visitor, svd );
  svd->curr_sym = prev_sym;

skip:
  return CXChildVisit_Continue;
}

/**
 * Compares two tidy_header objects.
 *
 * @note The \ref tidy_header::tsid "tsid" field is intentionally not compared.
 *
 * @param i_hdr The first tidy_header.
 * @param j_hdr The second tidy_header.
 * @return Returns a number less than 0, 0, or greater than 0 if \a i_hdr is
 * less than, equal to, or greater than \a j_hdr, respectively.
 */
NODISCARD
static int tidy_header_cmp( tidy_header const *i_hdr,
                            tidy_header const *j_hdr ) {
  int const cmp = tidy_FileUniqueID_cmp( &i_hdr->fuid, &j_hdr->fuid );
  if ( cmp != 0 )
    return cmp;
  return (i_hdr->lang > j_hdr->lang) - (i_hdr->lang < j_hdr->lang);
}

/**
 * Calculates the hash of \a hdr.
 *
 * @note The \ref tidy_header::tsid "tsid" field is intentionally not part of
 * the hash.
 *
 * @param hdr The tidy_header to calculate the hash for.
 * @return Returns said hash.
 */
NODISCARD
static ht_hash_val_t tidy_header_hash( tidy_header const *hdr ) {
  fnv1a_t hash = tidy_FileUniqueID_hash( &hdr->fuid );
  hash = FNV1A_PRIME * (hash ^ STATIC_CAST( fnv1a_t, hdr->lang ));
  return STATIC_CAST( ht_hash_val_t, hash );
}

/**
 * Visits a `CXCursor_Constructor` kind of cursor.
 *
 * @param ctor_csr The constructor's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_Constructor( CXCursor ctor_csr, symbols_visit_data *svd ) {
  CXCursor const cls_csr = clang_getCursorSemanticParent( ctor_csr );
  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    assert( svd != NULL );
    svd_cxx_constructor const prev_svd_cxx_constructor = svd->cxx_constructor;
    svd->cxx_constructor.ctor_csr = ctor_csr;
    tidy_visitClassChildren( cls_csr, &cxx_def_ctor_add_ref_visitor, svd );
    svd->cxx_constructor = prev_svd_cxx_constructor;
  }
}

/**
 * Visits a `CXCursor_CXXForRangeStmt` kind of cursor.
 *
 * @remarks In a C++ for-range statement, if the object being iterated over is
 * of a class type, we have to add references either to the class's `begin()`
 * and `end()` member functions or `begin(T [const]&)` and `end(T [const]&)`
 * global functions.
 *
 * @param range_stmt_csr The range-for statement's cursor.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXForRangeStmt( CXCursor range_stmt_csr,
                                   symbols_visit_data *svd ) {
  CXCursor range_expr = tidy_Cursor_getFirstExprChild( range_stmt_csr );
  if ( clang_Cursor_isNull( range_expr ) )
    return;

  CXType range_type = clang_getCursorType( range_expr );
  bool is_range_const = clang_isConstQualifiedType( range_type );

  range_type = clang_getCanonicalType( range_type );
  range_type = clang_getNonReferenceType( range_type );
  if ( !is_range_const )
    is_range_const = clang_isConstQualifiedType( range_type );

  CXCursor cls_csr = clang_getTypeDeclaration( range_type );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  cls_csr = clang_getCanonicalCursor( cls_csr );
  svd_cxx_for_range const prev_svd_cxx_for_range = svd->cxx_for_range;
  svd->cxx_for_range = (svd_cxx_for_range){
    .cls_csr = cls_csr,
    .begin_csr = clang_getNullCursor(),
    .end_csr = clang_getNullCursor(),
    .is_const = is_range_const
  };

  //
  // Try looking for T::{ begin | end }() [const] member functions first.
  //
  tidy_visitClassChildren( cls_csr, &cxx_find_begin_end_visitor, svd );

  //
  // If either is not found, try looking for { begin | end }(T [const]&)
  // non-member functions second.
  //
  if ( clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) ||
       clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
    svd->cxx_for_range.begin_csr = clang_getNullCursor();
    svd->cxx_for_range.end_csr = clang_getNullCursor();

    CXCursor scope_csr = clang_getCursorSemanticParent( cls_csr );
    while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
      clang_visitChildren( scope_csr, &cxx_find_begin_end_visitor, svd );
      if ( !clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) &&
           !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
        break;
      }
      enum CXCursorKind const kind = clang_getCursorKind( scope_csr );
      if ( kind == CXCursor_TranslationUnit )
        break;
      scope_csr = clang_getCursorSemanticParent( scope_csr );
    } // while
  }

  if ( !clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) ) {
    cursor_add_ref( svd->cxx_for_range.begin_csr, svd );
    assert( !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) );
    cursor_add_ref( svd->cxx_for_range.end_csr, svd );
  }

  svd->cxx_for_range = prev_svd_cxx_for_range;
}

/**
 * Visits a `CXCursor_CXXDeleteExpr` kind of cursor.
 *
 * @param delete_csr The `delete` cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXDeleteExpr( CXCursor delete_csr,
                                 symbols_visit_data *svd ) {
  clang_visitChildren( delete_csr, &cxx_DeleteExpr_visitor, svd );
}

/**
 * Visits a `CXCursor_CXXMethod` or `CXCursor_Destructor` kind of cursor.
 *
 * @note In what follows, "function" includes destructor.
 *
 * @remarks When a C++ virtual member function is called, libclang generates an
 * AST containing only the base class's function since it can't know which
 * overridden functions are actually called at run-time.  We therefore have to
 * add references from the base class's function to all the overridden ones.
 *
 * @param method_csr The method's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXMethod( CXCursor method_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor *overridden_csrs;
  unsigned  overridden_csrs_count;
  clang_getOverriddenCursors(
    method_csr, &overridden_csrs, &overridden_csrs_count
  );

  for ( unsigned i = 0; i < overridden_csrs_count; ++i ) {
    tidy_symbol *const base_sym = symbol_add( overridden_csrs[i], svd->source );
    if ( base_sym != NULL )
      symbol_add_ref( base_sym, svd->curr_sym );
    //
    // Standard library code can call a virtual function on an object of a
    // user-defined derived class, but nothing in the AST shows the call.
    // Therefore, add a reference from the class to the override so it will be
    // marked as used if the class is.
    //
    CXCursor const canon_csr = clang_getCanonicalCursor( overridden_csrs[i] );
    if ( tidy_Cursor_isInSystemHeader( canon_csr ) ) {
      CXCursor const parent_sym = clang_getCursorSemanticParent( method_csr );
      tidy_symbol *const cls_sym = symbol_add( parent_sym, svd->source );
      if ( cls_sym != NULL ) {
        symbol_add_ref( cls_sym, svd->curr_sym );
      }
      else {
        // If the class was excluded, just mark the method as a root.
        svd->curr_sym->is_root = true;
      }
    }
  } // for

  clang_disposeOverriddenCursors( overridden_csrs );

  cxx_member_def_ctor_add_ref( method_csr, svd->curr_sym, svd );

  enum CXCursorKind const kind = clang_getCursorKind( method_csr );
  switch ( kind ) {
    case CXCursor_CXXMethod:
      if ( tidy_Cursor_isSpellingEqualTo( method_csr, "operator()" ) ) {
        //
        // Libclang's C API doesn't include template instantiations of functors
        // (such as in STL containers) in the AST, so user-defined operator()
        // never get marked as used.
        //
        // As a hack, mark such operators as roots to ensure they (and any
        // symbols they reference) are marked as used.
        //
        svd->curr_sym->is_root = true;
      }
      break;

    case CXCursor_Destructor:
      //
      // A destructor implicitly calls its fields' and base classes'
      // destructors, but libclang's C API gives no reference for them.
      //
      cxx_dtor_member_add_refs( method_csr, svd );
      break;

    default:
      /* suppress warning */;
  } // switch
}

/**
 * Visits a `CXCursor_CXXNewExpr` kind of cursor.
 *
 * @remarks The class's `operator new` is implicitly called, so libclang's C
 * API gives no reference to it.  `operator delete` is also marked since it is
 * called if the constructor throws.
 *
 * @param new_csr The `new` cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXNewExpr( CXCursor new_csr, symbols_visit_data *svd ) {
  CXType type = clang_getCursorType( new_csr );
  type = clang_getCanonicalType( type );
  if ( type.kind != CXType_Pointer )
    return;

  CXType pointee_type = clang_getPointeeType( type );
         pointee_type = clang_getCanonicalType( pointee_type );

  CXCursor const cls_csr = clang_getTypeDeclaration( pointee_type );
  if ( tidy_Cursor_isClassDecl( cls_csr ) )
    cxx_operator_add_refs( cls_csr, "operator new", "operator new[]", svd );
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
 * @param svd The symbols_visit_data to use.
 */
static void visit_FieldDecl( CXCursor field_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXSourceRange const field_range = tidy_getCursorExtent( field_csr );
  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( field_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tu, field_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor          cls_csr = clang_getCursorSemanticParent( field_csr );
  CXString const    field_name_cxs = clang_getCursorSpelling( field_csr );
  char const *const field_name_cs  = clang_getCString( field_name_cxs );

  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;
    if ( tidy_Token_isSpellingEqualTo( tu, tokens[i], field_name_cs ) )
      continue;

    CXCursor const sym_csr = tidy_Token_getScopedNameCursor(
      tu, tokens, token_count, &i, cls_csr, svd->source->lang
    );
    cursor_add_ref( sym_csr, svd );
  } // for

  clang_disposeTokens( tu, tokens, token_count );

  if ( svd->source->lang == CXLanguage_CPlusPlus ) {
    unsigned const prev_num_refs = svd->curr_sym->ref_sym_set.size;

    cxx_field_def_ctor_add_ref( field_csr, svd );
    cxx_dtors_add_refs( field_csr, svd );

    //
    // If a reference was added to a constructor or destructor of the field's
    // type, either might have side effects, so the field must be considered
    // used if its class is.
    //
    tidy_symbol *const cls_sym = svd->curr_sym->parent_sym;
    if ( cls_sym != NULL && svd->curr_sym->ref_sym_set.size > prev_num_refs )
      symbol_add_ref( cls_sym, svd->curr_sym );
  }
}

/**
 * Visits a `CXCursor_FunctionDecl` kind of cursor in user code.
 *
 * @remarks For C++ only, records the user-defined classes among the function's
 * parameter types so that cxx_symbols_mark_adl_roots() can later determine
 * whether template code in a standard header could call the function via ADL.
 *
 * @param fn_csr The function's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_FunctionDecl( CXCursor fn_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  if ( svd->source->lang != CXLanguage_CPlusPlus || svd->curr_sym == NULL )
    return;

  CXType const fn_type = clang_getCursorType( fn_csr );
  int const num_params = clang_getNumArgTypes( fn_type );
  for ( int i = 0; i < num_params; ++i ) {
    CXType const arg_type = clang_getArgType( fn_type, (unsigned)i );
    cxx_adl_collect_classes(
      arg_type, /*skip_top=*/false, &svd->curr_sym->cxx_adl_usr_set
    );
  } // for
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
 * @param svd The symbols_visit_data to use.
 */
static void visit_MacroDefinition( CXCursor macro_csr, 
                                   symbols_visit_data *svd ) {
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
    if ( !tidy_Cursor_isInvalid( sym_csr ) )
      cursor_add_ref( sym_csr, svd );
  } // for

  ht_table_cleanup( &param_set, /*free_fn=*/NULL );
  clang_disposeTokens( svd->source->tu, tokens, token_count );
}

/**
 * Visits most kinds of reference cursor since they are handled the same way.
 *
 * @param cursor The cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_most_Ref( CXCursor cursor, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor const ref_csr = clang_getCursorReferenced( cursor );
  if ( tidy_Cursor_isInvalid( ref_csr ) ) {
    if ( svd->source->lang == CXLanguage_CPlusPlus ) {
      enum CXCursorKind const kind = clang_getCursorKind( cursor );
      if ( kind == CXCursor_MemberRefExpr )
        cxx_member_add_ref( cursor, svd );
    }
    return;
  }

  tidy_symbol *const ref_sym = symbol_add( ref_csr, svd->source );
  if ( ref_sym != NULL ) {
    symbol_add_ref( svd->curr_sym, ref_sym );
    if ( svd->source->lang == CXLanguage_CPlusPlus )
      cxx_member_def_ctor_add_ref( ref_csr, ref_sym, svd );
  }

  if ( svd->curr_sym == NULL )
    return;

  CXCursor const sem_parent = clang_getCursorSemanticParent( ref_csr );
  enum CXCursorKind const sem_parent_kind = clang_getCursorKind( sem_parent );
  if ( sem_parent_kind == CXCursor_EnumDecl ) {
    //
    // Special case: if any enumeration constant is referenced, add a reference
    // to the enumeration declaration so if any enumeration constant is used,
    // the declaration will be also.
    //
    cursor_add_ref( sem_parent, svd );
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
 * @param is_in_system_header `true` only if \a overloaded_csr is in a standard
 * header.
 * @param svd The symbols_visit_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr,
                                     bool is_in_system_header,
                                     symbols_visit_data *svd ) {
  assert( svd != NULL );
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  if ( is_in_system_header ) {
    //
    // This is (likely) a dependent call in a standard template.  Its callee
    // may be found by ADL at instantiation time among user functions that
    // we'll never see referenced; remember its name.  See
    // cxx_symbols_mark_adl_roots().
    //
    CXString const    name_cxs = clang_getCursorSpelling( overloaded_csr );
    char const *const name = clang_getCString( name_cxs );
    if ( name != NULL && name[0] != '\0' )
      cxx_adl_name_set_add( name );
    clang_disposeString( name_cxs );
  }

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    cursor_add_ref( dec_csr, svd );
  } // for
}

/**
 * Visits a `CXCursor_VarDeclRef` kind of cursor.
 *
 * @remarks For C++ only, if a variable of a class type is declared and the
 * class has a constructor, mark the variable as a root since the constructor
 * may have side effects.
 *
 * @param var_decl_csr The variable declaration's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_VarDecl( CXCursor var_decl_csr, symbols_visit_data *svd ) {
  if ( svd->source->lang != CXLanguage_CPlusPlus )
    return;

  CXCursor const cls_csr = tidy_Cursor_getUnderlyingTypeDecl( var_decl_csr );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  tidy_ConstructorInfo ctor_info;
  bool const has_ctor = tidy_Cursor_getConstructorInfo( cls_csr, &ctor_info );

  unsigned const prev_num_refs =
    svd->curr_sym != NULL ? svd->curr_sym->ref_sym_set.size : 0;

  //
  // Get whether the variable is explicitly initialized.  Given:
  //
  //      struct c_int {
  //        int _n;
  //      };
  //
  //      struct cxx_int {
  //        cxx_int( int n ) : _n{ n } { }
  //        int _n;
  //      };
  //
  // then for these cases:
  //                                    // has_ctor? num_args is_init_explicit?
  //      c_int c = { 42 };             //   false     -1       false
  //      cxx_int x;                    //   true       0       false
  //      cxx_int y{ 42 };              //   true       1       true
  //      cxx_int z = cxx_int{ 42 };    //   true      -1       true
  //
  CXCursor const init_csr = tidy_Cursor_getFirstExprChild( var_decl_csr );
  int const num_args = clang_Cursor_getNumArguments( init_csr );
  bool const is_init_explicit = has_ctor && num_args != 0;

  if ( !is_init_explicit ) {
    if ( has_ctor && !clang_Cursor_isNull( ctor_info.def_ctor_csr ) ) {
      //
      // Its class has a default constructor: add a reference to it.
      //
      cursor_add_ref( ctor_info.def_ctor_csr, svd );
    }
    else {
      //
      // Its class has no default constructor, but it might have members that
      // have default constructors: add references to those, if any.
      //
      tidy_visitClassChildren(
        cls_csr, &cxx_field_def_ctor_add_ref_visitor, svd
      );
    }
  }

  //
  // If the variable's type is a template specialization (e.g., std::vector<S>),
  // add default constructor references for any template type arguments.
  //
  CXType const var_type = clang_getCursorType( var_decl_csr );
  cxx_var_template_args_def_ctor_add_ref( var_type, svd );

  //
  // Add references to the variable's class's destructor and its base class
  // destructors, if any.
  //
  cxx_dtors_add_refs( var_decl_csr, svd );

  //
  // If the variable was either explicitly initialized or a reference was added
  // to a default constructor or destructor, the variable has to be treated as
  // used even if it's apparently unused since its constructor or destructor
  // might have side effects (e.g., locking a mutex).
  //
  if ( is_init_explicit || svd->curr_sym->ref_sym_set.size > prev_num_refs ) {
    tidy_symbol *const parent_sym = svd->curr_sym->parent_sym;
    if ( parent_sym != NULL && tidy_Cursor_inFunctionScope( var_decl_csr ) ) {
      //
      // The constructor and destructor run only if the enclosing function
      // does, so add a reference from the function to the variable.
      //
      symbol_add_ref( parent_sym, svd->curr_sym );
    }
    else {
      //
      // The variable has static storage (global, namespace-scope, or class
      // `static`), so its constructor and destructor run regardless.
      //
      svd->curr_sym->is_root = true;
    }
  }
}

////////// extern functions ///////////////////////////////////////////////////

void cursor_add_ref( CXCursor to_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  tidy_symbol *const to_sym = symbol_add( to_csr, svd->source );
  if ( to_sym != NULL )
    symbol_add_ref( svd->curr_sym, to_sym );
}

tidy_symbol* symbol_add( CXCursor sym_csr, tidy_source const *source ) {
  assert( source != NULL );

  sym_csr = clang_getCanonicalCursor( sym_csr );
  if ( cursor_is_excluded( sym_csr, source ) )
    return NULL;

  char *key = tidy_getCursorUSR( sym_csr );
  if ( unlikely( key == NULL ) )
    return NULL;

  tidy_symbol new_sym = {
    .key = key,
    .name = tidy_Cursor_getScopedSpelling( sym_csr )
  };
  key = NULL;                           // new_sym owns this now
  ht_insert_rv_t const hti =
    ht_table_insert( &tidy_symbol_set, &new_sym, sizeof new_sym );
  tidy_symbol *const sym = HT_DINT( hti.entry );
  if ( !hti.inserted ) {
    symbol_cleanup( &new_sym );
    goto done;
  }

  if ( source->lang == CXLanguage_CPlusPlus )
    sym->cxx_prototype = tidy_Cursor_getScopedDisplayName( sym_csr );

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

  if ( tidy_Cursor_isEnclosingScope( sym_csr ) )
    sym->is_scope = true;

  ht_table_init(
    &sym->ref_sym_set, HT_DPTR, 2.0, 128,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

done:
  free( key );
  return sym;
}

void symbol_add_ref( tidy_symbol *from_sym, tidy_symbol *to_sym ) {
  assert( to_sym != NULL );

  if ( from_sym == NULL )
    to_sym->is_root = true;
  else
    ht_table_insert( &from_sym->ref_sym_set, to_sym, 0 );
}

void symbols_init( void ) {
  ASSERT_RUN_ONCE();
  ht_table_init(
    &header_set, HT_DINT, 2.0, 256,
    POINTER_CAST( ht_cmp_fn_t, &tidy_header_cmp ),
    POINTER_CAST( ht_hash_fn_t, &tidy_header_hash )
  );
  ht_table_init(
    &tidy_symbol_set, HT_DINT, 2.0, 16384,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &symbol_hash )
  );
  cxx_symbols_init();
  ATEXIT( &symbols_cleanup );
}

unsigned symbols_print_unused( void ) {
  unsigned const unused = symbols_mark_used();
  if ( unused == 0 )
    return 0;

  array_t symbol_array = ARRAY_INIT( sizeof(tidy_symbol) );
  array_reserve( &symbol_array, unused );

  ht_iterator_t iter;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( !sym->is_used && !sym->is_dead )
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
      EPRINTF( "\"%s\"", sym->path );
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
    EPRINTF( " | \"%s\"\n", symbol_name_for_print( sym ) );
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
    .cxx_constructor = {
      .ctor_csr = clang_getNullCursor()
    },
    .cxx_for_range = {
      .cls_csr = clang_getNullCursor(),
      .begin_csr = clang_getNullCursor(),
      .end_csr = clang_getNullCursor()
    }
  };
  CXCursor const cursor = clang_getTranslationUnitCursor( source->tu );
  clang_visitChildren( cursor, &symbols_visitor, &svd );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
