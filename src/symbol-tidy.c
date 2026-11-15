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
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "str_util.h"
#include "symbol.h"
#include "util.h"
#include "verbose.h"

// standard
#include <stdlib.h>
#include <sysexits.h>

////////// enums //////////////////////////////////////////////////////////////

/**
 * **include-tidy**-specific exit status codes.
 */
enum {
  TIDY_EX_VIOLATIONS          = 1,      ///< One or more violations.
  TIDY_EX_NO_VIOLATIONS_ERROR = 2       ///< No violations, but error anyway.
};

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

char const   *prog_name;

/// @endcond

////////// local functions ////////////////////////////////////////////////////

/**
 * Prints a summary line only if any if the parameters values are &gt; 0.
 *
 * @param unused_symbols The number of unused symbols.
 * @param unneeded_forwards The number of unneeded declarations.
 * @param unneeded_externs The number of unneeded external declarations.
 */
static void print_summary( unsigned unused_symbols, unsigned unneeded_forwards,
                           unsigned unneeded_externs ) {
  if ( unused_symbols == 0 && unneeded_forwards == 0 &&
       unneeded_externs == 0 ) {
    return;
  }

  char const *sep ="";

  EPRINTF( "%s: ", prog_name );

  if ( unused_symbols > 0 ) {
    EPRINTF( "%u unused symbol%s", unused_symbols, plural_s( unused_symbols ) );
    sep = "; ";
  }

  if ( unneeded_forwards > 0 ) {
    EPRINTF(
      "%s%u unneeded declaration%s",
      sep, unneeded_forwards, plural_s( unneeded_forwards )
    );
    sep = "; ";
  }

  if ( unneeded_externs > 0 ) {
    EPRINTF( "%s%u unneeded external linkage", sep, unneeded_externs );
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
void test_init( char const *env_var ) {
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
 * @param violations_count The number of violations.
 * @return Returns said exit status.
 */
NODISCARD
static int tidy_status( unsigned violations_count ) {
  if ( opt_error != TIDY_ERROR_NEVER ) {
    if ( violations_count > 0 )
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
  config_init();
  sources_init();
  symbols_init();

  if ( IS_VERBOSE( SRC_FILE_ALWAYS ) )
    verbose_section_begin( /*printed_header=*/NULL );

  unsigned unneeded_forwards = 0;

  for ( size_t i = 0; i < sources.len; ++i ) {
    tidy_source *const source = array_at_nc( &sources, i );
    if ( !source_path_add( source->path ) )
      continue;

    if ( IS_VERBOSE( SRC_FILE_ALWAYS ) )
      verbose_printf( "source file = \"%s\"\n", source->path );

    source_parse( source );
    unneeded_forwards += symbols_visit( source );
    source_cleanup( source );
  } // for

  unsigned const unused_symbols = symbols_print_unused();
  unsigned const unneeded_externs = symbols_print_unneeded_external();

  array_cleanup( &sources, /*free_fn=*/NULL );

  print_summary( unused_symbols, unneeded_forwards, unneeded_externs );
  return tidy_status( unused_symbols + unneeded_forwards + unneeded_externs );
}
