/*
**      symbol-tidy -- Symbol tidier
**      src/symbol-tidy.c
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
**      along with this program.  If not, see <http
*/

/**
 * @file
 * Defines `main()`, a function for exit status, and global variables.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "symbol-tidy.h"
#include "array.h"
#include "cli_options.h"
#include "color.h"
#include "config_file.h"
#include "fwd_dec.h"
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "str_util.h"
#include "symbol.h"
#include "util.h"
#include "verbose.h"

// standard
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

////////// enums //////////////////////////////////////////////////////////////

/**
 * **symbol-tidy**-specific exit status codes.
 */
enum {
  TIDY_EX_VIOLATIONS          = 1,      ///< One or more violations.
  TIDY_EX_NO_VIOLATIONS_ERROR = 2       ///< No violations, but error anyway.
};

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct tidy_summary tidy_summary;

////////// structs ////////////////////////////////////////////////////////////

/**
 * Values for the summary line.
 */
struct tidy_summary {
  unsigned undefined_symbols;     ///< Number of undefined symbols.
  unsigned unneeded_externs;      ///< Number of unneeded extern declarations.
  unsigned unneeded_forwards;     ///< Number of unneeded forward declarations.
  unsigned unused_symbols;        ///< Number of unused symbols.
  unsigned violations_count;      ///< Total number of violations.
};

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

char const   *prog_name;

/// @endcond

////////// local functions ////////////////////////////////////////////////////

/**
 * Prints a summary line only if any values of the summary are &gt; 0.
 *
 * @param summary The tidy_summary to check.
 */
static void summary_print( tidy_summary const *summary ) {
  assert( summary != NULL );

  if ( summary->violations_count == 0 )
    return;

  char const *sep ="";

  EPRINTF( "%s: ", prog_name );

  // These are printed in "importance" order.

  if ( summary->unused_symbols > 0 ) {
    EPRINTF( "%u unused symbol%s",
    summary->unused_symbols, plural_s( summary->unused_symbols ) );
    sep = "; ";
  }

  if ( summary->undefined_symbols > 0 ) {
    EPRINTF(
      "%s%u undefined symbol%s",
      sep, summary->undefined_symbols, plural_s( summary->undefined_symbols )
    );
    sep = "; ";
  }

  if ( summary->unneeded_forwards > 0 ) {
    EPRINTF(
      "%s%u unneeded forward%s",
      sep, summary->unneeded_forwards, plural_s( summary->unneeded_forwards )
    );
    sep = "; ";
  }

  if ( summary->unneeded_externs > 0 ) {
    EPRINTF( "%s%u unneeded external linkage", sep, summary->unneeded_externs );
    sep = "; ";
  }

  EPUTC( '\n' );
}

/**
 * Initializes testing.
 *
 * @param env_var The name of the environment variable containing a test format
 * string (case sensitive) to parse.
 */
static void test_init( char const *env_var ) {
  char const *const value = empty_if_null( getenv( env_var ) );
  if ( !opt_test_parse( value ) ) {
    // LCOV_EXCL_START
    fatal_error( EX_USAGE,
      "\"%s\": invalid value for %s; must be [" OPT_TEST_ALL "]|*|-\n",
      value, env_var
    );
    // LCOV_EXCL_STOP
  }
}

/**
 * Gets the status **symbol-tidy** should exit with.
 *
 * @param summary The tidy_summary to use.
 * @return Returns said exit status.
 */
NODISCARD
static int tidy_status( tidy_summary const *summary ) {
  assert( summary != NULL );

  if ( opt_error != TIDY_ERROR_NEVER ) {
    if ( summary->violations_count > 0 )
      return TIDY_EX_VIOLATIONS;
    if ( opt_error == TIDY_ERROR_ALWAYS )
      return TIDY_EX_NO_VIOLATIONS_ERROR;
  }
  return EX_OK;
}

////////// extern functions ///////////////////////////////////////////////////

/**
 * The main entry point.
 *
 * @param argc The command-line argument count.
 * @param argv The command-line argument values.
 * @return Returns 0 on success, non-zero on failure.
 */
int main( int argc, char const *argv[] ) {
  prog_name = path_basename( argv[0] );

  // Initialization MUST happen in this order.
  test_init( "SYMBOL_TIDY_TEST" );
  options_init();
  array_t sources = ARRAY_INIT( sizeof( tidy_source ) );
  cli_options_init( argc, argv, &sources );
  colors_init();
  if ( opt_read_config )
    config_init();
  sources_init();
  symbols_init();

  unsigned unneeded_forwards = 0;

  for ( size_t i = 0; i < sources.len; ++i ) {
    tidy_source *const source = array_at_nc( &sources, i );
    if ( !source_path_add( source->path ) )
      continue;

    if ( IS_VERBOSE( SRC_FILE_PATH ) ) {
      verbose_new_line();
      verbose_printf( "source file = \"%s\"\n", source->path );
      fflush( stdout );
    }

    symbols_visit_data svd;
    svd_init( &svd, source );

    source_parse( &svd );
    symbols_visit( &svd );
    if ( !svd.ignore_forwards )
      unneeded_forwards += fwd_decs_print_unneeded( svd.source );
    source_cleanup( source );
  } // for

  unsigned const unused_symbols = symbols_mark_used();
  if ( unused_symbols > 0 )
    symbols_print_unused( unused_symbols );

  unsigned const undefined_symbols = symbols_print_undefined();
  unsigned const unneeded_externs = symbols_print_unneeded_external();

  array_cleanup( &sources, /*free_fn=*/NULL );

  tidy_summary const summary = {
    .undefined_symbols = undefined_symbols,
    .unneeded_externs = unneeded_externs,
    .unneeded_forwards = unneeded_forwards,
    .unused_symbols = unused_symbols,
    .violations_count =
      + undefined_symbols
      + unneeded_externs
      + unneeded_forwards
      + unused_symbols
  };
  summary_print( &summary );
  return tidy_status( &summary );
}
