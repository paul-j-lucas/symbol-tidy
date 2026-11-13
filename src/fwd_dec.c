/*
**      symbol-tidy -- Symbol tidier
**      src/fwd_dec.c
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
 * Defines functions for finding and printing unnecessary forward function
 * declarations within a single translation unit.
 */

// local
#include "pjl_config.h"
#include "fwd_dec.h"
#include "array.h"
#include "clang_util.h"
#include "color.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "symbol-tidy.h"
#include "symbol.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/// @endcond

/**
 * @addtogroup tidy-fwd-dec-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct fwd_dec        fwd_dec;
typedef struct fwd_statistics fwd_statistics;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A possibly unnecessary forward declaration.
 *
 * @remarks All `*_visit_id` members are values of \ref
 * symbols_visit_data::visit_id, which starts at 1, so 0 means "not seen."
 */
struct fwd_dec {
  CXCursor  canon_csr;                  ///< Canonical cursor (the hash key).
  CXCursor  dec_csr;                    ///< The forward declaration itself.
  unsigned  dec_visit_id;               ///< Where it was declared.
  CXCursor  def_csr;                    ///< The definition, if any.
  unsigned  def_visit_id;               ///< Where it was defined, if at all.
  unsigned  first_ref_visit_id;         ///< Where first referenced, if at all.
  bool      has_attributes;             ///< Declaration has attributes?
  bool      in_linkage_spec;            ///< Declaration in linkage spec?
  bool      is_internal_linkage;        ///< Has internal linkage?
  bool      is_redundant;               ///< Is this declaration redundant?
};

/**
 * Forward declaration statistics (in addition to the typical ones for
 * fwd_dec_set).
 */
struct fwd_statistics {
  /// The maximum load factor of \ref fwd_dec_set among all translation units.
  double  max_lf;

  /// The path of the translation unit that yielded \ref max_lf.
  char   *max_lf_path;

  /// The maximum size of \ref fwd_dec_set among all translation units.
  size_t  max_size;

  /// The path of the translation unit that yielded \ref max_size.
  char   *max_size_path;
};

////////// local functions ////////////////////////////////////////////////////

static void         fwd_statistics_cleanup( fwd_statistics* );
static void const*  ht_table_get_any( hash_table_t const* );
static void         print_statistics( verbose_stat_ctx* );

////////// local variables ////////////////////////////////////////////////////

/**
 * Redundant redeclarations for the current translation unit, i.e., ones that
 * are never a function's first declaration.
 */
static array_t        fwd_dec_redundant = ARRAY_INIT( sizeof(fwd_dec) );

/**
 * The set of possibly unnecessary forward declarations for the current
 * translation unit.
 */
static hash_table_t   fwd_dec_set;

static fwd_statistics fwd_stats;        ///< Forward declaration statistics.

////////// local functions ////////////////////////////////////////////////////

/**
 * Compares two fwd_dec objects by their canonical cursors.
 *
 * @param i_fwd The first fwd_dec.
 * @param j_fwd The second fwd_dec.
 * @return Returns 0 only if the cursors are equal.
 */
NODISCARD
static int fwd_dec_cmp( fwd_dec const *i_fwd, fwd_dec const *j_fwd ) {
  return !clang_equalCursors( i_fwd->canon_csr, j_fwd->canon_csr );
}

/**
 * Compares two fwd_dec objects by where they were declared.
 *
 * @param i_fwd The first fwd_dec.
 * @param j_fwd The second fwd_dec.
 * @return Returns an integer less than, equal to, or greater than 0.
 */
NODISCARD
static int fwd_dec_cmp_by_visit_id( fwd_dec const *i_fwd,
                                    fwd_dec const *j_fwd ) {
  assert( i_fwd != NULL );
  assert( j_fwd != NULL );

  if ( i_fwd->dec_visit_id < j_fwd->dec_visit_id )
    return -1;
  if ( i_fwd->dec_visit_id > j_fwd->dec_visit_id )
    return 1;
  return 0;
}

/**
 * Finds the fwd_dec for \a canon_csr, if any.
 *
 * @param canon_csr The canonical cursor to find.
 * @return Returns the fwd_dec or NULL if there isn't one.
 */
NODISCARD
static fwd_dec* fwd_dec_find( CXCursor canon_csr ) {
  if ( fwd_dec_set.size == 0 )
    return NULL;
  fwd_dec const find_fwd = { .canon_csr = canon_csr };
  ht_entry_t const *const entry = ht_table_find( &fwd_dec_set, &find_fwd );
  return entry != NULL ? HT_DINT( entry ) : NULL;
}

/**
 * Hashes a fwd_dec by its canonical cursor.
 *
 * @param fwd The fwd_dec to hash.
 * @return Returns said hash.
 */
NODISCARD
static fnv1a_t fwd_dec_hash( fwd_dec const *fwd ) {
  return clang_hashCursor( fwd->canon_csr );
}

/**
 * Gets whether \a fwd is a necessary forward declaration.
 *
 * @remarks A forward declaration is necessary if a function is defined in the
 * same translation unit and is referenced between the declaration and its
 * definition.
 *
 * @note A function with internal linkage and no references at all is _not_
 * reported since it will be reported as an unused symbol.
 *
 * @param fwd The fwd_dec to check.
 * @return Returns `true` only if \a fwd is necessary.
 */
NODISCARD
static bool fwd_dec_is_necessary( fwd_dec const *fwd ) {
  if ( fwd->def_visit_id == 0 ) {
    //
    // The function isn't defined in this translation unit.
    //
    if ( fwd->first_ref_visit_id == 0 ) {
      //
      // Nothing references it, so the declaration is unnecessary.
      //
      return true;
    }
    else {
      //
      // It's referenced, so the declaration is necessary.
      //
      // For a function with external linkage, it's presumably defined in
      // another translation unit; for one with internal linkage, the missing
      // definition is a link-time error.
      //
      return false;
    }
  }

  //
  // A definition always comes after a declaration. A fwd_dec is created when a
  // function's first declaration is visited, and fwd_dec_visit_def() ignores
  // definitions that come before it because no fwd_dec exists yet.
  //
  assert( fwd->def_visit_id > fwd->dec_visit_id );

  if ( fwd->first_ref_visit_id == 0 ) {
    //
    // The function is never referenced in this translation unit.
    //
    if ( fwd->is_internal_linkage ) {
      //
      // Since the function has internal linkage, it's never used, so it will
      // be reported as an unused symbol, so return true here to pretend it's
      // necessary so it won't also be reported as unnecessary, i.e., reported
      // twice.
      //
      return true;
    }
    else {
      //
      // Since it has external linkage, another translation unit may reference
      // it, so it may not reported as unused, so return the truth that its
      // forward declaration is really unnecessary.
      //
      return false;
    }
  }

  if ( fwd->first_ref_visit_id > fwd->def_visit_id ) {
    //
    // The function is referenced and defined in this translation unit, and
    // the first (earliest) reference comes after the definition, so no
    // reference between the declaration and the definition needs the
    // declaration to be visible.
    //
    return false;
  }

  //
  // The first reference comes before the definition, so the declaration is
  // necessary.
  //
  return true;
}

/**
 * Cleans-up all forward declarations.
 */
static void fwd_decs_cleanup( void ) {
  verbose_print_statistics( &print_statistics );
  fwd_statistics_cleanup( &fwd_stats );
  ht_table_cleanup( &fwd_dec_set, /*free_fn=*/NULL );
}

/**
 * Resets forward declarations for another translation unit.
 */
static void fwd_decs_reset( void ) {
  array_cleanup( &fwd_dec_redundant, /*free_fn=*/NULL );

  if ( fwd_dec_set.size == 0 )
    return;
  if ( !IS_VERBOSE( STATISTICS ) )
    goto skip;

  fwd_dec const *fwd = NULL;

  double const lf = ht_table_load_factor( &fwd_dec_set );
  if ( lf > fwd_stats.max_lf ) {
    fwd_stats.max_lf = lf;
    fwd = ht_table_get_any( &fwd_dec_set );
    free( fwd_stats.max_lf_path );
    fwd_stats.max_lf_path = tidy_Cursor_dupFileName( fwd->dec_csr );
  }

  if ( fwd_dec_set.size > fwd_stats.max_size ) {
    fwd_stats.max_size = fwd_dec_set.size;
    if ( fwd == NULL )
      fwd = ht_table_get_any( &fwd_dec_set );
    free( fwd_stats.max_size_path );
    fwd_stats.max_size_path = tidy_Cursor_dupFileName( fwd->dec_csr );
  }

skip:
  ht_table_clear( &fwd_dec_set, /*free_fn=*/NULL );
}

/**
 * Cleans up forward declaration statistics.
 *
 * @param stats The fwd_statistics to clean-up.
 */
static void fwd_statistics_cleanup( fwd_statistics *stats ) {
  if ( stats != NULL ) {
    free( stats->max_lf_path );
    free( stats->max_size_path );
    *stats = (fwd_statistics){ 0 };
  }
}

/**
 * Gets the data of an arbitrary entry of \a table.
 *
 * @param table The hash table to get an entry of.
 * @return Returns a pointer to the data of an arbitrary entry of \a table or
 * NULL if \a table is empty.
 */
static void const* ht_table_get_any( hash_table_t const *table ) {
  assert( table != NULL );
  ht_iterator_t iter;
  ht_iterator_init( &iter, table );
  return ht_iterator_next( &iter );
}

/**
 * Prints statistics for forward declarations.
 *
 * @param vsc The verbose_stat_ctx to use.
 */
static void print_statistics( verbose_stat_ctx *vsc ) {
  verbose_statf( vsc, "forward declaration set:\n" );
  verbose_statf( vsc,
    "fds-max-load-factor = " TIDY_STAT_LF_FMT "\n", fwd_stats.max_lf
  );
  verbose_statf( vsc,
    "fds-max-load-factor-path \"%s\"\n", fwd_stats.max_lf_path
  );
  verbose_statf( vsc, "fds-max-size = %zu\n", fwd_stats.max_size );
  verbose_statf( vsc, "fds-max-size-path \"%s\"\n", fwd_stats.max_size_path );
}

////////// extern functions ///////////////////////////////////////////////////

void fwd_dec_maybe_unnecessary( CXCursor fn_csr,
                                symbols_visit_data const *svd ) {
  assert( svd != NULL );

  CXSourceLocation const fn_loc = clang_getCursorLocation( fn_csr );
  if ( !clang_Location_isFromMainFile( fn_loc ) )
    return;
  if ( !tidy_Cursor_inFileScope( fn_csr ) )
    return;
  if ( tidy_Cursor_inMacro( fn_csr ) )
    return;

  CXCursor const canon_csr = clang_getCanonicalCursor( fn_csr );
  CXSourceLocation const canon_loc = clang_getCursorLocation( canon_csr );
  bool const is_redundant = !clang_equalCursors( fn_csr, canon_csr );

  if ( is_redundant && !clang_Location_isFromMainFile( canon_loc ) )
    return;

  fwd_dec new_fwd = {
    .canon_csr = canon_csr,
    .dec_csr = fn_csr,
    .dec_visit_id = svd->visit_id,
    .has_attributes = tidy_Cursor_hasAnyAttr( fn_csr ),
    .in_linkage_spec = tidy_Cursor_inLinkageSpec( fn_csr ),
    .is_internal_linkage = !tidy_Cursor_isExternalLinkage( fn_csr ),
    .is_redundant = is_redundant
  };

  if ( is_redundant ) {
    new_fwd.def_csr = canon_csr;
    *(fwd_dec*)array_push_back( &fwd_dec_redundant ) = new_fwd;
  }
  else {
    ht_table_insert( &fwd_dec_set, &new_fwd, sizeof new_fwd );
  }
}

void fwd_dec_visit_ref( CXCursor ref_csr, symbols_visit_data const *svd ) {
  assert( svd != NULL );
  if ( fwd_dec_set.size == 0 )
    return;                             // fast path: no candidates

  ref_csr = clang_getCanonicalCursor( ref_csr );
  fwd_dec *const fwd = fwd_dec_find( ref_csr );
  if ( fwd != NULL && fwd->first_ref_visit_id == 0 )
    fwd->first_ref_visit_id = svd->visit_id;
}

void fwd_dec_visit_def( CXCursor fn_csr, symbols_visit_data const *svd ) {
  if ( fwd_dec_set.size == 0 )
    return;                             // fast path: no candidates

  CXCursor const canon_fn_csr = clang_getCanonicalCursor( fn_csr );
  fwd_dec *const fwd = fwd_dec_find( canon_fn_csr );
  if ( fwd != NULL && fwd->def_visit_id == 0 ) {
    assert( svd != NULL );
    fwd->def_visit_id = svd->visit_id;
    fwd->def_csr = fn_csr;
  }
}

void fwd_decs_init( void ) {
  ASSERT_RUN_ONCE();
  ht_table_init(
    &fwd_dec_set, HT_DINT, 2.0, 64,
    POINTER_CAST( ht_cmp_fn_t, &fwd_dec_cmp ),
    POINTER_CAST( ht_hash_fn_t, &fwd_dec_hash )
  );
  ATEXIT( &fwd_decs_cleanup );
}

unsigned fwd_decs_print_unnecessary( tidy_source const *source ) {
  assert( source != NULL );

  unsigned unnecessary_count =
    STATIC_CAST( unsigned, fwd_dec_redundant.len );

  ht_iterator_t iter;
  ht_iterator_init( &iter, &fwd_dec_set );
  for ( fwd_dec const *fwd; (fwd = ht_iterator_next( &iter )) != NULL; ) {
    if ( !fwd_dec_is_necessary( fwd ) )
      ++unnecessary_count;
  } // for

  if ( unnecessary_count == 0 )
    goto done;

  array_t fwd_array = ARRAY_INIT( sizeof(fwd_dec) );
  array_reserve( &fwd_array, unnecessary_count );

  for ( size_t i = 0; i < fwd_dec_redundant.len; ++i ) {
    fwd_dec const *const fwd = array_at_nc( &fwd_dec_redundant, i );
    *(fwd_dec*)array_push_back( &fwd_array ) = *fwd;
  } // for

  ht_iterator_init( &iter, &fwd_dec_set );
  for ( fwd_dec const *fwd; (fwd = ht_iterator_next( &iter )) != NULL; ) {
    if ( !fwd_dec_is_necessary( fwd ) )
      *(fwd_dec*)array_push_back( &fwd_array ) = *fwd;
  } // for

  array_qsort(
    &fwd_array, POINTER_CAST( array_cmp_fn_t, &fwd_dec_cmp_by_visit_id )
  );

  fwd_dec const    *fwd = array_front_nc( &fwd_array );
  CXFile const      dec_file = tidy_getCursorLocation_File( fwd->dec_csr );
  CXString const    dec_path_cxs = clang_getFileName( dec_file );
  char const *const dec_path_cs = clang_getCString( dec_path_cxs );

  color_start( stderr, sgr_locus );
  EPRINTF( "\"%s\"", path_no_dot_slash( dec_path_cs ) );
  color_end( stderr, sgr_locus );
  clang_disposeString( dec_path_cxs );

  EPUTS( ": " );
  color_start( stderr, sgr_warning );
  EPUTS( "warning" );
  color_end( stderr, sgr_warning );
  EPUTS( ": unnecessary forward declarations:\n" );

  for ( size_t i = 0; i < fwd_array.len; ++i ) {
    fwd = array_at_nc( &fwd_array, i );

    CXSourceLocation const dec_loc = clang_getCursorLocation( fwd->dec_csr );
    unsigned dec_line, dec_col;
    clang_getSpellingLocation(
      dec_loc, /*file=*/NULL, &dec_line, &dec_col, /*offset=*/NULL
    );

    char *const fn_name = source->lang == CXLanguage_CPlusPlus ?
      tidy_Cursor_getScopedDisplayName( fwd->dec_csr ) :
      tidy_Cursor_getScopedSpelling( fwd->dec_csr );
    assert( fn_name != NULL );

    color_start( stderr, sgr_locus );
    EPRINTF( "%5u", dec_line );
    color_end( stderr, sgr_locus );
    EPUTC( ',' );
    color_start( stderr, sgr_locus );
    EPRINTF( "%2u", dec_col );
    color_end( stderr, sgr_locus );

    EPRINTF( " | \"%s\"", fn_name );
    free( fn_name );

    if ( fwd->def_visit_id == 0 || fwd->is_redundant )
      goto next;

    bool const move_attributes = fwd->has_attributes;
    bool const move_linkage_spec =
      fwd->in_linkage_spec && !tidy_Cursor_inLinkageSpec( fwd->def_csr );

    if ( move_attributes || move_linkage_spec ) {
      CXSourceLocation const def_loc = clang_getCursorLocation( fwd->def_csr );
      unsigned const def_line = tidy_getSpellingLocation_Line( def_loc );
      EPUTS( " (move " );
      if ( move_attributes )
        EPUTS( "attributes" );
      if ( move_attributes && move_linkage_spec )
        EPUTS( " and " );
      if ( move_linkage_spec )
        EPUTS( "linkage specification" );
      EPUTS( " to definition at line " );
      color_start( stderr, sgr_locus );
      EPRINTF( "%u", def_line );
      color_end( stderr, sgr_locus );
      EPUTC( ')' );
    }

next:
    EPUTC( '\n' );
  } // for

  array_cleanup( &fwd_array, /*free_fn=*/NULL );

done:
  fwd_decs_reset();
  return unnecessary_count;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
