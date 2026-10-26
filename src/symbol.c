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
  tidy_symbol        *curr_sym;         ///< Current symbol, if any.

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
    CXCursor          cls_csr;          ///< Class being iterated over.
    CXCursor          begin_csr;        ///< Found `begin()` cursor, if any.
    CXCursor          end_csr;          ///< Found `end()` cursor, if any.
    bool              is_const;         ///< Is the range-for `const`?
  } cxx_for_range;
};

////////// local functions ////////////////////////////////////////////////////

static void cxx_dtor_add_ref( CXCursor, symbols_visit_data const* );
static void symbol_add_ref( tidy_symbol*, tidy_symbol* );
NODISCARD
static int  symbol_cmp( tidy_symbol const*, tidy_symbol const* );
static void symbol_cleanup( tidy_symbol* );

NODISCARD
static bool symbol_is_excluded( CXCursor, tidy_source const* );

static void visit_Constructor( CXCursor, symbols_visit_data* );
static void visit_CXXDeleteExpr( CXCursor, symbols_visit_data* );
static void visit_CXXForRangeStmt( CXCursor, symbols_visit_data* );
static void visit_CXXMethod( CXCursor, symbols_visit_data* );
static void visit_FieldDecl( CXCursor, symbols_visit_data* );
static void visit_MacroDefinition( CXCursor, symbols_visit_data* );
static void visit_most_Ref( CXCursor, symbols_visit_data* );
static void visit_OverloadedDeclRef( CXCursor, symbols_visit_data* );
static void visit_VarDecl( CXCursor, symbols_visit_data* );

////////// local variables ////////////////////////////////////////////////////

static hash_table_t symbol_set;         ///< Set of symbols.

////////// local functions ////////////////////////////////////////////////////

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

  ht_table_init(
    &sym->ref_sym_set, HT_DPTR, 2.0, 128,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

done:
  free( key );
  free( sym_name );
  return sym;
}

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
 * A helper function for visit_Constructor() that visits all base classes of \a
 * cursor looking for an explicit call to a base class's constructor: if none
 * is found, find the base class's default constructor and add a reference to
 * it.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 */
static
enum CXChildVisitResult cxx_def_ctor_add_ref_visitor( CXCursor cursor,
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

  assert( svd->curr_sym != NULL );
  symbol_add_ref( svd->curr_sym, def_ctor_sym );

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
 * Helper function for visit_CXXDeleteExpr() that adds a reference to the
 * destructor of the object's class.
 *
 * @param cursor The cursor to visit.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Recurse`.
 */
static enum CXChildVisitResult cxx_DeleteExpr_visitor( CXCursor cursor,
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
    return CXChildVisit_Break;
  }

skip:
  return CXChildVisit_Recurse;
}

/**
 * Adds a reference to the destructor of \a cls_csr if it has one.
 *
 * @param cls_csr The cursor for the class.
 * @param svd The symbols_visit_data to use.
 */
static void cxx_dtor_add_ref( CXCursor cls_csr,
                              symbols_visit_data const *svd ) {
  assert( svd != NULL );

  CXCursor const dtor_csr = tidy_Cursor_getDestructor( cls_csr );
  if ( tidy_Cursor_isInvalid( dtor_csr ) )
    return;

  tidy_symbol *const dtor_sym = add_symbol( dtor_csr, svd->source );
  if ( dtor_sym != NULL )
    symbol_add_ref( svd->curr_sym, dtor_sym );
}

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
static void cxx_member_add_def_ctor_ref( CXCursor member_csr,
                                         tidy_symbol *member_sym,
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
  if ( clang_Cursor_isNull( ctor_info.default_ctor_csr ) )
    return;

  tidy_symbol *const def_ctor_sym =
    add_symbol( ctor_info.default_ctor_csr, svd->source );
  if ( def_ctor_sym != NULL )
    symbol_add_ref( member_sym, def_ctor_sym );
}

/**
 * A helper function for visit_CXXForRangeStmt() that attempts to find
 * `begin()` and `end()` functions.
 *
 * @param cursor The cursor being visited.
 * @param parent Not used.
 * @param data The symbols_visit_data to use.
 * @return Returns either `CXChildVisit_Break` or `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult find_begin_end_visitor( CXCursor cursor,
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

    case CXCursor_FunctionDecl:         // func( [const] T& )
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

  tidy_symbol const *max_sym = NULL;
  double max_lf = -1.0;

  // Find the symbol having the ref_sym_set with the largest load factor.
  ht_iterator_t iter;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_dead )
      continue;
    double const lf = ht_table_load_factor( &sym->ref_sym_set );
    if ( lf > max_lf ) {
      max_sym = sym;
      max_lf = lf;
    }
  } // for

  if ( max_sym != NULL ) {
    verbose_printf(
      "    ss-max-load-factor:\n"
    );
    verbose_printf(
      "      ss-name = \"%s\"\n",
      max_sym->display_name ? max_sym->display_name : max_sym->name
    );
    if ( max_sym->path != NULL ) {
      verbose_printf(
        "      ss-path = \"%s\"\n", max_sym->path
      );
    }
    verbose_printf(
      "      ref. symbol set:\n"
    );
    verbose_printf(
      "        rss-load-factor = " TIDY_STAT_LF_FMT "\n", max_lf
    );
    verbose_printf(
      "        rss-size = %u\n", max_sym->ref_sym_set.size
    );
  }
}

/**
 * Adds a reference from \a from_sym to \a to_sym, or mark \a to_sym as a \ref
 * tidy_symbol::is_root "root".
 *
 * @param from_sym The symbol to add a reference from. If NULL, \a to_sym is
 * marked as a \ref tidy_symbol::is_root "root".
 * @param to_sym The symbol to add a reference to or be marked as a \ref
 * tidy_symbol::is_root "root".
 */
static void symbol_add_ref( tidy_symbol *from_sym, tidy_symbol *to_sym ) {
  assert( to_sym != NULL );

  if ( from_sym == NULL )
    to_sym->is_root = true;
  else
    ht_table_insert( &from_sym->ref_sym_set, to_sym, 0 );
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
    case CXCursor_Namespace:
      //
      // Namespaces first declared in system headers (e.g., "std") should be
      // excluded even if re-opened in user code.
      //
      sym_csr = clang_getCanonicalCursor( sym_csr );
      CXSourceLocation const loc = clang_getCursorLocation( sym_csr );
      return clang_Location_isInSystemHeader( loc );

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
  (void)parent;
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  CXSourceLocation const sym_loc = clang_getCursorLocation( cursor );
  bool const is_in_system_header = clang_Location_isInSystemHeader( sym_loc );

  if ( is_in_system_header ) {
    //
    // Symbols in system headers can be skipped entirely in C.
    //
    if ( svd->source->lang == CXLanguage_C )
      goto skip;
    //
    // In C++, they can't be skipped because template code in system headers
    // calls back into user code, so we need it to mark the user code as used.
    //
  }
  else if ( IS_VERBOSE( CURSORS ) ) {
    verbose_print_cursor( cursor );     // LCOV_EXCL_LINE
  }

  tidy_symbol *const prev_sym = svd->curr_sym;

  bool is_definition = false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );

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

    case CXCursor_EnumConstantDecl:
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
      visit_OverloadedDeclRef( cursor, svd );
      break;

    default:
      /* suppress warning */;
  } // switch

  if ( is_definition ) {
    tidy_symbol *const sym = add_symbol( cursor, svd->source );
    if ( likely( sym != NULL ) ) {
      if ( tidy_Cursor_isEnclosingScope( cursor ) )
        sym->is_scope = true;

      if ( is_in_system_header ) {
        //
        // As mentioned above, in C++, template code in system headers calls
        // back into user code, so mark all symbols in system headers as roots.
        //
        sym->is_root = true;
      }
      else {
        //
        // For definitions, set the symbol's path, line, and column.
        //
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

  clang_visitChildren( cursor, symbols_visitor, svd );
  svd->curr_sym = prev_sym;

skip:
  return CXChildVisit_Continue;
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
    clang_visitChildren( cls_csr, &cxx_def_ctor_add_ref_visitor, svd );
    svd->cxx_constructor = prev_svd_cxx_constructor;
  }
}

/**
 * Visits a `CXCursor_CXXForRangeStmt` kind of cursor.
 *
 * @param range_stmt_csr The range-for statement's cursor.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXForRangeStmt( CXCursor range_stmt_csr,
                                   symbols_visit_data *svd ) {
  CXCursor range_expr = clang_getNullCursor();
  clang_visitChildren( range_stmt_csr, &get_Expr_visitor, &range_expr );
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

  // Try looking for T::{ begin | end }() [const] member functions first.
  clang_visitChildren( cls_csr, &find_begin_end_visitor, svd );

  // If either is not found, try looking for { begin | end }([const] T&)
  // non-member functions second.
  if ( clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) ||
       clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
    svd->cxx_for_range.begin_csr = clang_getNullCursor();
    svd->cxx_for_range.end_csr = clang_getNullCursor();

    CXCursor scope_csr = clang_getCursorSemanticParent( cls_csr );
    while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
      clang_visitChildren( scope_csr, &find_begin_end_visitor, svd );
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
    tidy_symbol *sym = add_symbol( svd->cxx_for_range.begin_csr, svd->source );
    if ( sym != NULL )
      symbol_add_ref( svd->curr_sym, sym );
    assert( !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) );
    sym = add_symbol( svd->cxx_for_range.end_csr, svd->source );
    if ( sym != NULL )
      symbol_add_ref( svd->curr_sym, sym );
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
 * AST containing only the base class's member function since it can't know
 * which overridden functions are actually called.  So as not to them as
 * unused, add references from the base one to all the overridden ones.
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
    tidy_symbol *const base_sym = add_symbol( overridden_csrs[i], svd->source );
    if ( base_sym != NULL )
      symbol_add_ref( base_sym, svd->curr_sym );
  } // for

  clang_disposeOverriddenCursors( overridden_csrs );

  cxx_member_add_def_ctor_ref( method_csr, svd->curr_sym, svd );

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

    default:
      /* suppress warning */;
  } // switch
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

  CXCursor const    cls_csr = clang_getCursorSemanticParent( field_csr );
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
    tidy_symbol *const sym = add_symbol( sym_csr, svd->source );
    if ( sym != NULL )
      symbol_add_ref( svd->curr_sym, sym );
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
    if ( !tidy_Cursor_isInvalid( sym_csr ) ) {
      tidy_symbol *const sym = add_symbol( sym_csr, svd->source );
      if ( sym != NULL )
        symbol_add_ref( svd->curr_sym, sym );
    }
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
  if ( unlikely( tidy_Cursor_isInvalid( ref_csr ) ) )
    return;

  tidy_symbol *const ref_sym = add_symbol( ref_csr, svd->source );
  if ( ref_sym != NULL ) {
    symbol_add_ref( svd->curr_sym, ref_sym );
    if ( svd->source->lang == CXLanguage_CPlusPlus )
      cxx_member_add_def_ctor_ref( ref_csr, ref_sym, svd );
  }

  if ( svd->curr_sym == NULL )
    return;

  CXCursor const sem_parent = clang_getCursorSemanticParent( ref_csr );
  enum CXCursorKind const sem_parent_kind = clang_getCursorKind( sem_parent );
  if ( sem_parent_kind == CXCursor_EnumDecl ) {
    //
    // Special case: if any enumeration constant is referenced (which means its
    // semantic parent is an EnumDecl), mark the enumeration as a whole as
    // used.
    //
    tidy_symbol *const enum_decl_sym = add_symbol( sem_parent, svd->source );
    if ( enum_decl_sym != NULL )
      symbol_add_ref( svd->curr_sym, enum_decl_sym );
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
 * @param svd The symbols_visit_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr,
                                     symbols_visit_data *svd ) {
  assert( svd != NULL );

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    tidy_symbol *const dec_sym = add_symbol( dec_csr, svd->source );
    if ( dec_sym != NULL )
      symbol_add_ref( svd->curr_sym, dec_sym );
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
    if ( !tidy_Token_isSpellingEqualTo( source->tu, tokens[i], "#" ) )
      continue;
    if ( ++i == token_count )
      break;

    token_kind = clang_getTokenKind( tokens[i] );
    if ( token_kind != CXToken_Identifier )
      continue;
    int const directive_idx = tidy_Token_isSpellingEqualToAny(
      source->tu, tokens[i],
      "if", "ifdef", "ifndef", "elif", "elifdef", "elifndef"
    );
    if ( directive_idx == -1 )
      continue;

    unsigned last_line = tidy_getSpellingLocation_Line( token_loc );

    while ( ++i < token_count ) {
      unsigned const token_line =
        tidy_getTokenLocation_Line( source->tu, tokens[i] );
      if ( token_line > last_line ) {
        if ( !tidy_isEscapedNewline( source->tu, source->text, tokens, i ) ) {
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

/**
 * Visits a `CXCursor_VarDeclRef` kind of cursor.
 *
 * @remarks For C++ only, if a variable of a class type is declared and the
 * class has a constructor, mark the variable as a root (used) since the
 * constructor may have side effects.
 *
 * @param var_decl_csr The variable declaration's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_VarDecl( CXCursor var_decl_csr, symbols_visit_data *svd ) {
  if ( svd->source->lang != CXLanguage_CPlusPlus )
    return;

  CXCursor  obj_csr = tidy_Cursor_getElementIfArray( var_decl_csr );
  CXType    type = clang_getCursorType( obj_csr );
            type = clang_getCanonicalType( type );
            type = clang_getNonReferenceType( type );

  CXCursor const cls_csr = clang_getTypeDeclaration( type );

  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    if ( tidy_Cursor_getConstructorInfo( cls_csr, /*ctor_info=*/NULL ) )
      svd->curr_sym->is_root = true;

    cxx_dtor_add_ref( cls_csr, svd );
    clang_visitChildren( cls_csr, &cxx_base_dtor_add_ref_visitor, svd );
  }
}

////////// extern functions ///////////////////////////////////////////////////

void symbols_init( void ) {
  ASSERT_RUN_ONCE();
  ht_table_init(
    &symbol_set, HT_DINT, 2.0, 16384,
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
  visit_preprocessor_directives( source );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
