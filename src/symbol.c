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
#include "cli_options.h"
#include "config_file.h"
#include "cxx.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "include.h"
#include "options.h"
#include "str_util.h"
#include "symbol-tidy.h"
#include "trans_unit.h"
#include "typedef.h"
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
typedef struct symbols_visitor_data symbols_visitor_data;
/// @endcond

////////// structs ////////////////////////////////////////////////////////////

/**
 * Additional data passed to symbols_init_visitor().
 */
struct symbols_visitor_data {
  tidy_symbol *curr_parent;
};

////////// local functions ////////////////////////////////////////////////////

static void     symbol_cleanup( tidy_symbol* );

NODISCARD
static bool     symbol_is_excluded( CXCursor );

NODISCARD
static bool     visit_CallExpr( CXCursor, CXCursor, symbols_init_data* );

static void     visit_FieldDecl( CXCursor, CXCursor, symbols_init_data* );
static void     visit_MacroDefinition( CXCursor, CXCursor, symbols_init_data* );
static void     visit_MemberRefExpr( CXCursor, CXCursor, symbols_init_data* );
static void     visit_most_kinds( CXCursor, CXCursor, symbols_init_data* );
static void     visit_OverloadedDeclRef( CXCursor, CXCursor,
                                         symbols_init_data* );

////////// local variables ////////////////////////////////////////////////////

static hash_table_t symbol_set;         ///< Set of symbols.

////////// local functions ////////////////////////////////////////////////////

/**
 * Adds a symbol to the global set and marks the header that declares is as
 * necessary.
 *
 * @param name_csr The cursor to use for the name of the symbol.  It may be
 * (and often is) the same as \a sym_csr.
 * @param sym_csr The cursor for the symbol to add.
 * @param sym_file The file that contains the declaration for \a sym_csr.
 * @param sid The symbols_init_data to use.
 *
 * @sa maybe_add_symbol()
 */
static void add_symbol( CXCursor sym_csr, CXFile sym_file,
                        symbols_visitor_data *svd ) {
  assert( svd != NULL );

  // TODO
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
 * @param sid The symbols_init_data to use.
 */
static void maybe_add_symbol( CXCursor name_csr, CXCursor sym_csr,
                              symbols_init_data *sid ) {
  if ( symbol_is_excluded( sym_csr ) )
    return;
  CXFile const sym_file = get_symbol_file( sym_csr, sid );
  if ( sym_file == NULL )
    return;
  add_symbol( name_csr, sym_csr, sym_file, sid );
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

void symbol_add_ref( tidy_symbol *from, tidy_symbol *to ) {
  assert( from != NULL );
  assert( to != NULL );

    /* Prevent duplicate refs */
  tidy_ref *ref = from->refs;
  while ( ref != NULL ) {
    if ( ref->target == to )
      return;
    ref = ref->next;
  }
  ref = MALLOC( tidy_ref, 1 );
  *ref = { .target = to, .next = from->refs };
  from->refs = ref;

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

    for ( tidy_ref *ref = sym->refs; ref != NULL; ) {
      tidy_ref *const next = ref->next;
      free( ref );
      ref = next;
    } // for
  }
}

/**
 * TODO
 *
 * @param sym TODO.
 */
static void symbol_mark_reachable( tidy_symbol *sym ) {
  assert( sym != NULL );

  if ( !sym->is_reachable ) {
    sym->is_reachable = true;
    for ( tidy_ref *ref = sym->refs; ref != NULL; ref = ref->next )
      symbol_mark_reachable( ref->target );
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
 * @param data A pointer to a symbols_init_data.
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
  symbols_visitor_data *const svd = data;

  bool is_def = false;
  enum CXCursorKind const kind = clang_getCursorKind( cursor );

  switch ( kind ) {
    case CXCursor_FunctionDecl:
    case CXCursor_VarDecl:;
      if ( !clang_isCursorDefinition( cursor ) )
        break;
      enum CXLinkageKind const linkage = clang_getCursorLinkage( cursor );
      switch ( linkage ) {
        case CXLinkage_External:
        case CXLinkage_UniqueExternal:
          is_def = true;
          break;
        default:
          /* suppress warning */;
      } // switch
      break;
    case CXCursor_MacroDefinition:
      is_def = true;
      break;
    default:
      /* suppress warning */;
  } // switch

  char *const usr = tidy_getCanonicalCursorUSR( cursor );

  if ( usr != NULL && is_def ) {
    tidy_symol ins_sym = { .key = usr };
    ht_insert_rv_t hti = ht_table_insert( table, &ins_sym, sizeof ins_sym );
    tidy_symbol *const sym = HT_DINT( hti.entry );
    if ( hti.inserted ) {
      sym->key = strdup_or_exit( usr );
      CXString const name_cxs = clang_getCursorSpelling( cursor );
      sym->name = strdup_or_exit( clang_getCString( name_cxs ) );
      clang_disposeString( name_cxs );
      if ( strcmp( sym->name, "main" ) == 0 )
        sym->is_root = true;
    }

    /* Update file/line to point to the actual definition */
    CXFile file;
    unsigned line, col;
    clang_getSpellingLocation( loc, &file, &line, &col, NULL );
    CXString cx_fname = clang_getFileName( file );
    char const *const fname = clang_getCString( cx_fname );
    if ( fname ) {
      free( sym->file );
      sym->file = strdup( fname );
      sym->line = line;
      sym->col = col;
    }
    clang_disposeString( cx_fname );

    if ( kind == CXCursor_FunctionDecl )
      next_data.current_parent = sym;
  }

  /* 2. Record usages/references using canonical target cursors */
  CXCursor ref;
  switch ( kind ) {
    case CXCursor_CallExpr:
    case CXCursor_DeclRefExpr:
    case CXCursor_MacroExpansion:
    case CXCursor_MemberRefExpr:
      ref = clang_getCursorReferenced( cursor );
      break;
    default:
      ref = clang_getNullCursor();
  } // switch

  if ( !clang_Cursor_isNull( ref ) &&
       !clang_isInvalid( clang_getCursorKind( ref ) ) ) {
    char *const ref_usr = get_canonical_usr( ref );
    if ( ref_usr != NULL ) {
      tidy_symbol *const target = get_or_create_node( visitor_data->table, ref_usr );
      if ( visitor_data->current_parent != NULL )
        symbol_add_ref( visitor_data->current_parent, target );
      else
        target->is_root = true;

      free( ref_usr );
    }
  }

  free( usr );

  clang_visitChildren( cursor, ast_visitor, &next_data );

skip:
  return CXChildVisit_Continue;
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
 * @param sid The symbols_init_data to use.
 */
static void visit_FieldDecl( CXCursor field_csr, CXCursor parent,
                             symbols_init_data *sid ) {
  (void)parent;
  assert( sid != NULL );

  CXSourceRange const field_range = tidy_getCursorExtent( field_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tidy_tu, field_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  CXString const    field_name_cxs = clang_getCursorSpelling( field_csr );
  char const *const field_name_cs  = clang_getCString( field_name_cxs );

  CXCursor const cls_csr = clang_getCursorSemanticParent( field_csr );

  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;

    CXString const    token_cxs = clang_getTokenSpelling( tidy_tu, tokens[i] );
    char const *const token_cs  = clang_getCString( token_cxs );
    bool const        is_field_name = strcmp( token_cs, field_name_cs ) == 0;

    clang_disposeString( token_cxs );

    if ( is_field_name )
      continue;

    CXCursor const sym_csr =
      tidy_Token_getScopedNameCursor( tokens, token_count, &i, cls_csr );
    maybe_add_symbol( sym_csr, sym_csr, sid );
  } // for

  clang_disposeTokens( tidy_tu, tokens, token_count );
}

/**
 * Visits a `CXCursor_MacroDefinition` kind of cursor.
 *
 * @remarks
 * @parblock
 * We have to iterate over all tokens of the macro's definition looking for
 * identifiers to see whether the header that declares them has been included.
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
 * @param sid The symbols_init_data to use.
 */
static void visit_MacroDefinition( CXCursor macro_csr, CXCursor parent,
                                   symbols_init_data *sid ) {
  (void)parent;
  assert( sid != NULL );

  CXSourceRange const macro_range = clang_getCursorExtent( macro_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tidy_tu, macro_range, &tokens, &token_count );
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
    macro_get_params( tokens, token_count, &param_set ) :
    1;                                  // tokens[0] = macro name; start at 1

  for ( ; i < token_count; ++i ) {
    CXCursor const sym_csr =
      macro_Token_getScopedNameCursor( tokens, token_count, &i, &param_set );
    maybe_add_symbol( sym_csr, sym_csr, sid );
  } // for

  ht_table_cleanup( &param_set, /*free_fn=*/NULL );
  clang_disposeTokens( tidy_tu, tokens, token_count );
}

/**
 * Visit most kinds of cursor.
 *
 * @param cursor The cursor to visit.
 * @param parent The parent cursor of \a cursor.
 * @param sid The symbols_init_data to use.
 */
static void visit_most_kinds( CXCursor cursor, CXCursor parent,
                              symbols_init_data *sid ) {
  assert( sid != NULL );

  // Gets the cursor for the declaration of the symbol.
  CXCursor const dec_csr = clang_getCursorReferenced( cursor );
  if ( tidy_Cursor_isInvalid( dec_csr ) )
    return;                             // LCOV_EXCL_LINE

  //
  // Call symbol_is_excluded() and get_symbol_file() ourselves to avoid calling
  // is_cxx_proxy_qual_ref_iwyu_exc() and is_cxx_mbr_or_base_iwyu_exc() unless
  // necessary since they're both expensive.
  //
  if ( !symbol_is_excluded( dec_csr ) ) {
    CXFile const dec_file = get_symbol_file( dec_csr, sid );
    if ( dec_file != NULL ) {
      if ( tidy_source_is_cxx ) {
        // See the comment for symbols_init_data::cxx_deferred_fn_csr.
        if ( clang_equalCursors( dec_csr, sid->cxx_deferred_fn_csr ) )
          return;

        CXCursor const scope_csr = sid_cxx_scope( sid, parent );
        if ( is_cxx_proxy_qual_ref_iwyu_exc( cursor, parent, scope_csr ) )
          return;
        if ( is_cxx_mbr_or_base_iwyu_exc( dec_csr, scope_csr ) )
          return;
      }
      add_symbol( dec_csr, dec_csr, dec_file, sid );
    }
  }

  CXCursor def_csr;
  if ( is_symbol_definition_needed( cursor, parent, dec_csr, &def_csr ) )
    maybe_add_symbol( dec_csr, def_csr, sid );
}

/**
 * Visits a `CXCursor_MemberRefExpr` kind of cursor.
 *
 * @param mbr_ref_csr The member reference's cursor to visit.
 * @param parent Not used.
 * @param sid The symbols_init_data to use.
 */
static void visit_MemberRefExpr( CXCursor mbr_ref_csr, CXCursor parent,
                                 symbols_init_data *sid ) {
  (void)parent;
  assert( sid != NULL );

  CXCursor const mbr_csr = clang_getCursorReferenced( mbr_ref_csr );
  if ( tidy_Cursor_isInvalid( mbr_csr ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor const mbr_cls_csr = clang_getCursorSemanticParent( mbr_csr );
  if ( !tidy_Cursor_isClassDecl( mbr_cls_csr ) )
    goto skip;

  // For a MemberRefExpr, the first child is the class/struct/union object that
  // we're referencing the member of.
  CXCursor const obj_csr = tidy_Cursor_getFirstExposedChild( mbr_ref_csr );
  if ( tidy_Cursor_isInvalid( obj_csr ) )
    goto skip;

  if ( tidy_source_is_cxx ) {
    if ( is_cxx_arrow_iwyu_exc( obj_csr, mbr_cls_csr ) )
      return;
    if ( is_cxx_mbr_ref_iwyu_exc( obj_csr ) )
      return;
  }

  CXCursor const type_csr = tidy_Cursor_getUnderlyingType( obj_csr );
  enum CXCursorKind const kind = clang_getCursorKind( type_csr );

  switch ( kind ) {
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
      //
      // This is for a case like:
      //
      //      // types.h
      //      #include <time.h>
      //      typedef struct timespec timespec_t;
      //
      //      // foo.c
      //      #include "types.h"
      //      void f( timespec_t *time ) {
      //        time_t t = time->tv_sec;
      //        // ...
      //      }
      //
      // Since types.h includes time.h, timespec_t is an alias for a complete
      // type since the definition of timespec has been seen.  Therefore, foo.c
      // need only include types.h and not time.h to access tv_sec (that
      // requires a complete type).
      //
      if ( !tidy_Cursor_isTypeComplete( type_csr ) )
        break;
      CXCursor const def_cls_csr = clang_getCursorDefinition( mbr_cls_csr );
      if ( tidy_Cursor_isInvalid( def_cls_csr ) )
        break;                          // LCOV_EXCL_LINE
      CXFile const def_file = tidy_getCursorLocation_File( def_cls_csr );
      if ( def_file == NULL )
        break;                          // LCOV_EXCL_LINE
      tidy_include const *const def_include = include_find_by_File( def_file );
      if ( def_include == NULL )
        break;                          // LCOV_EXCL_LINE
      if ( def_include->depth > 0 )     // not directly included
        return;
      break;

    default:
      /* suppress warning */;
  } // switch

skip:
  visit_most_kinds( mbr_ref_csr, mbr_cls_csr, sid );
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
 * @param sid The symbols_init_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr, CXCursor parent,
                                     symbols_init_data *sid ) {
  (void)parent;
  assert( sid != NULL );

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    maybe_add_symbol( dec_csr, dec_csr, sid );
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
  ht_iterator_init( &iter, &symbols_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_root )
      symbol_mark_reachable( sym );
  } // for

  unsigned unused = 0;
  ht_iterator_init( &iter, &symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_reachable )
      continue;

    if ( is_input_file( sym->file, argc, argv ) ) {
      printf( "%s:%d:%d: unused symbol '%s'\n",
              sym->file, sym->line, sym->col, sym->name );
      ++unused;
    }
  } // for

  return unused;
}

void symbols_visit( tidy_source *source ) {
  symbols_visitor_data svd = { 0 };
  CXCursor const cursor = clang_getTranslationUnitCursor( source->tu );
  clang_visitChildren( cursor, &symbols_visitor, &svd );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
