/*
**      symbol-tidy -- Symbol tidier
**      src/options.c
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
 * Defines types, global variables, and functions for **include-tidy**(1)
 * options.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "options.h"
#include "color.h"
#include "type_traits.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>                     /* for exit() */
#include <string.h>                     /* for str...() */
#include <strings.h>                    /* for strcasecmp() */
#include <sysexits.h>

/// @endcond

/**
 * @addtogroup options-group
 * @{
 */

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

color_when    opt_color_when = COLOR_NOT_FILE;
bool          opt_config_layers = true;
char const   *opt_config_path;
bool          opt_debug;
tidy_error    opt_error;
tidy_test     opt_test;
tidy_verbose  opt_verbose;

/// @endcond

/////////// local functions ///////////////////////////////////////////////////

/**
 * Parses a string into an `unsigned long long`.
 *
 * @remarks Unlike **strtoull**(3), insists that \a s is entirely a non-
 * negative integer.
 *
 * @param s The NULL-terminated string to parse.
 * @return Returns the parsed integer only if \a s is entirely a non-negative
 * integer or prints an error message and exits if there was an error.
 */
NODISCARD
static unsigned long long parse_ull( char const *s ) {
  assert( s != NULL );
  SKIP_WS( s );
  if ( likely( s[0] != '\0' && s[0] != '-' ) ) {
    char *end = NULL;
    errno = 0;
    unsigned long long const n = strtoull( s, &end, 0 );
    if ( likely( errno == 0 && *end == '\0' ) )
      return n;
  }
  fatal_error( EX_USAGE, "\"%s\": invalid integer\n", s );
}

////////// extern functions ///////////////////////////////////////////////////

bool opt_color_parse( char const *s ) {
  struct color_when_map {
    char const *when_str;
    color_when  when;
  };
  typedef struct color_when_map color_when_map;

  static color_when_map const COLOR_WHEN_MAP[] = {
    { "always",    COLOR_ALWAYS   },
    { "auto",      COLOR_ISATTY   },    // grep compatibility
    { "isatty",    COLOR_ISATTY   },    // explicit synonym for auto
    { "never",     COLOR_NEVER    },
    { "not_file",  COLOR_NOT_FILE },    // !ISREG( stdout )
    { "not_isreg", COLOR_NOT_FILE },    // synonym for not_isfile
    { "tty",       COLOR_ISATTY   },    // synonym for isatty
  };

  assert( s != NULL );

  FOREACH_ARRAY_ELEMENT( color_when_map, m, COLOR_WHEN_MAP ) {
    if ( strcasecmp( s, m->when_str ) == 0 ) {
      opt_color_when = m->when;
      return true;
    }
  } // for

  return false;
}

bool opt_error_parse( char const *s ) {
  struct error_map {
    char const *error_str;
    tidy_error  error;
  };
  typedef struct error_map error_map;

  static error_map const ERROR_MAP[] = {
    { "always",     TIDY_ERROR_ALWAYS         },
    { "never",      TIDY_ERROR_NEVER          },
    { "violations", TIDY_ERROR_IF_VIOLATIONS  },
  };

  assert( s != NULL );

  FOREACH_ARRAY_ELEMENT( error_map, m, ERROR_MAP ) {
    if ( strcasecmp( s, m->error_str ) == 0 ) {
      opt_error = m->error;
      return true;
    }
  } // for

  return false;
}

bool opt_test_parse( char const *test_format ) {
  assert( test_format != NULL );

  option_str_set_all_or_none( &test_format, OPT_TEST_ALL );
  tidy_test test = TIDY_TEST_NONE;

  for ( char const *s = test_format; *s != '\0'; ++s ) {
    switch ( *s ) {
      case 'e':
        test |= TIDY_TEST_NO_ETC_XDG;
        break;
      case 'h':
        test |= TIDY_TEST_NO_HOME;
        break;
      default:
        return false;                   // LCOV_EXCL_LINE
    } // switch
  } // for

  opt_test = test;
  return true;
}

bool opt_verbose_parse( char const *verbose_format ) {
  assert( verbose_format != NULL );

  option_str_set_all_or_none( &verbose_format, OPT_VERBOSE_ALL );
  tidy_verbose verbose = TIDY_VERBOSE_NONE;

    for ( char const *s = verbose_format; *s != '\0'; ++s ) {
    switch ( *s ) {
      case 'a':
        // LCOV_EXCL_START: -I, -isystem, etc., options contain absolute paths.
        verbose |= TIDY_VERBOSE_ARGS;
        break;
        // LCOV_EXCL_STOP
      case 'c':
        // LCOV_EXCL_START: config. files are printed as absolute paths.
        verbose |= TIDY_VERBOSE_CONFIG_FILES;
        break;
        // LCOV_EXCL_STOP
      case 'C':
        // LCOV_EXCL_START: function cursors have platform-specific signatures.
        verbose |= TIDY_VERBOSE_CURSORS;
        break;
        // LCOV_EXCL_STOP
      case 'd':
        verbose |= TIDY_VERBOSE_DIRECTORY;
        break;
      case 'f':
        verbose |= TIDY_VERBOSE_SRC_FILE_VIOLATIONS;
        break;
      case 'F':
        verbose |= TIDY_VERBOSE_SRC_FILE_ALWAYS;
        break;
      case 's':
        verbose |= TIDY_VERBOSE_SYMBOLS;
        break;
      case 'S':
        // LCOV_EXCL_START: even though a symbol specifies a fixed set of
        // headers it's declared it, an actual dump is the subset of those
        // headers that are actually used.
        verbose |= TIDY_VERBOSE_CONFIG_SYMBOLS;
        break;
        // LCOV_EXCL_STOP
      case 'z':
        verbose |= TIDY_VERBOSE_STATISTICS;
        break;
      default:
        return false;
    } // switch
  } // for

  opt_verbose = verbose;

  if ( IS_VERBOSE( SRC_FILE_ALWAYS ) )
    verbose &= ~TO_UNSIGNED_EXPR( TIDY_VERBOSE_SRC_FILE_VIOLATIONS );

  return true;
}

void option_str_set_all_or_none( char const **pformat, char const *all_value ) {
  assert( pformat != NULL );
  assert( *pformat != NULL );
  assert( all_value != NULL );
  assert( all_value[0] != '\0' );

  if ( strcmp( *pformat, "*" ) == 0 )
    *pformat = all_value;               // LCOV_EXCL_LINE
  else if ( strcmp( *pformat, "-" ) == 0 )
    *pformat = "";
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
