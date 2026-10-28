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
#include "cli_options.h"
#include "color.h"
#include "config_file.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "str_util.h"
#include "symbol.h"
#include "util.h"
#include "verbose.h"

// standard
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

////////// enums //////////////////////////////////////////////////////////////

/**
 * **include-tidy**-specific exit status codes.
 */
enum {
  TIDY_EX_UNUSED          = 1,    ///< One or more unused symbols.
  TIDY_EX_NO_UNUSED_ERROR = 2     ///< No unused symbols, but error anyway.
};

////////// local variables ////////////////////////////////////////////////////

static hash_table_t argv_path_set;      ///< Source files being tidied.

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

char const   *prog_name;

/// @endcond

////////// local functions ////////////////////////////////////////////////////

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
 * Cleans-up **symbol-tidy**.
 */
static void tidy_cleanup( void ) {
  ht_table_cleanup( &argv_path_set, /*free_fn=*/NULL );
}

/**
 * Gets the status **symbol-tidy** should exit with.
 *
 * @param unused_symbols The number of unused symbold.
 * @return Returns said exit status.
 */
NODISCARD
static int tidy_status( unsigned unused_symbols ) {
  if ( opt_error != TIDY_ERROR_NEVER ) {
    if ( unused_symbols > 0 )
      return TIDY_EX_UNUSED;
    if ( opt_error == TIDY_ERROR_ALWAYS )
      return TIDY_EX_NO_UNUSED_ERROR;
  }
  return EX_OK;
}

////////// extern functions ///////////////////////////////////////////////////

bool is_argv_path( char const *path ) {
  assert( path != NULL );
  ht_entry_t const *const found_ht = ht_table_find( &argv_path_set, path );
  return found_ht != NULL;
}

/**
 * The main entry point.
 *
 * @param argc The command-line argument count.
 * @param argv The command-line argument values.
 * @return Returns 0 on success, non-zero on failure.
 */
int main( int argc, char const *argv[] ) {
  ATEXIT( &tidy_cleanup );
  prog_name = path_basename( argv[0] );

  // Initialization MUST happen in this order.
  test_init( "SYMBOL_TIDY_TEST" );
  options_init();
  int const arg_source1 = cli_options_init( &argc, &argv );
  colors_init();
  config_init();
  symbols_init();

  ht_table_init(
    &argv_path_set, HT_DPTR, 2.0, 256,
    POINTER_CAST( ht_cmp_fn_t, strcmp ),
    POINTER_CAST( ht_hash_fn_t, fnv1a_s )
  );

  if ( IS_VERBOSE( SRC_FILE_ALWAYS ) )
    verbose_section_begin( /*printed_header=*/NULL );

  for ( int i = arg_source1; i < argc; ++i ) {
    if ( config_path_is_ignored( argv[i] ) )
      continue;
    ht_insert_rv_t const hti = ht_table_insert(
      &argv_path_set, CONST_CAST( char*, argv[i] ), strlen( argv[i] ) + 1
    );
    if ( !hti.inserted )
      continue;

    if ( IS_VERBOSE( SRC_FILE_ALWAYS ) )
      verbose_printf( "source file = \"%s\"\n", argv[i] );

    tidy_source source = {
      .argv_path = argv[i],
      .lang = opt_lang
    };
    source_init( &source, arg_source1, argv );
    symbols_visit( &source );
    source_cleanup( &source );
  } // for

  unsigned const unused = symbols_print_unused();
  return tidy_status( unused );
}
