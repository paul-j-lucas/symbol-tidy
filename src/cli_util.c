/*
**      symbol-tidy -- Symbol tidier
**      src/cli_util.c
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
 * Defines functions for command-line utilities.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "cli_util.h"
#include "cli_options.h"
#include "str_util.h"
#include "strbuf.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <getopt.h>
#include <stdlib.h>                     /* for exit() */
#include <string.h>                     /* for str...() */
#include <sysexits.h>

/// @endcond

/**
 * @addtogroup cli-util-group
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

char const* get_long_opt_value( int argc, char const *const argv[],
                                char const *long_opt, int *pargi ) {
  assert(     argc > 0 );
  assert(     argv != NULL );
  assert( long_opt != NULL );
  assert(    pargi != NULL );
  assert(   *pargi > 0 );

  char const *const arg = argv[ *pargi ];

  if ( STRNCMPLIT( arg, "--" ) != 0 )
    return NULL;
  size_t const long_opt_len = strlen( long_opt );
  if ( strncmp( arg + STRLITLEN( "--" ), long_opt, long_opt_len ) != 0 )
    return NULL;
  if ( arg[ STRLITLEN( "--" ) + long_opt_len ] == '=' ) {
    char const *const value = arg + STRLITLEN( "--" ) + long_opt_len + 1;
    if ( value[0] != '\0' )
      return value;
  }
  else if ( ++*pargi < argc ) {
    char const *const value = argv[ *pargi ];
    if ( !null_or_empty( value ) && value[0] != '-' )
      return value;
  }

  fatal_error( EX_USAGE, "\"--%s\" requires an argument\n", long_opt );
}

char const* get_opt_format( int short_opt ) {
  static strbuf_t sbufs[2];
  static unsigned buf_index;

  strbuf_t *const sbuf = &sbufs[ buf_index++ % ARRAY_SIZE( sbufs ) ];
  strbuf_reset( sbuf );

  struct option const *const option = get_option_short( short_opt );
  char const *const long_opt = option != NULL ? option->name : "";
  return strbuf_printf(
    sbuf, "%s%s%s-%c",
    long_opt[0] != '\0' ? "--" : "",
    long_opt,
    long_opt[0] != '\0' ? "/" : "",
    short_opt
  );
}

struct option const* get_option_long( char const *long_opt ) {
  assert( long_opt != NULL );
  char const *const equal = strchr( long_opt, '=' );
  size_t const n = equal != NULL ?
    STATIC_CAST( size_t, equal - long_opt ) : strlen( long_opt );

  FOREACH_CLI_OPTION( option ) {
    if ( strncmp( option->name, long_opt, n ) == 0 )
      return option;
  } // for
  return NULL;
}

struct option const* get_option_short( int short_opt ) {
  assert( short_opt > 0 && short_opt < 128 );
  FOREACH_CLI_OPTION( option ) {
    if ( option->val == short_opt )
      return option;
  } // for
  return NULL;
}

char const* get_short_opt_value( int argc, char const *const argv[],
                                 int short_opt, int *pargi ) {
  assert(   argc > 0 );
  assert(   argv != NULL );
  assert(  pargi != NULL );
  assert( *pargi > 0 );
  assert( short_opt > 0 && short_opt < 128 );

  if ( argv[ *pargi ][0] != '-' || argv[ *pargi ][1] != short_opt )
    return NULL;
  if ( argv[ *pargi ][2] != '\0' )    // -x<arg>, not -x <arg>
    return argv[ *pargi ] + 2;
  if ( ++*pargi < argc ) {
    char const *const value = argv[ *pargi ];
    if ( !null_or_empty( value ) && value[0] != '-' )
      return value;
  }

  fatal_error( EX_USAGE, "\"-%c\" requires an argument\n", short_opt );
}

char* make_short_opts( void ) {
  // pre-flight to calculate string length
  unsigned len = 1;                     // for leading ':'
  FOREACH_CLI_OPTION( option ) {
    assert( option->has_arg >= 0 && option->has_arg <= 2 );
    len += 1 + STATIC_CAST( unsigned, option->has_arg );
  } // for

  char *const short_opts = MALLOC( char, len + 1/*\0*/ );
  char *s = short_opts;

  *s++ = ':';                           // return missing argument as ':'
  FOREACH_CLI_OPTION( option ) {
    assert( option->val > 0 && option->val < 128 );
    *s++ = STATIC_CAST( char, option->val );
    switch ( option->has_arg ) {
      case optional_argument:
        // LCOV_EXCL_START: tidy doesn't currently have any optional arguments.
        *s++ = ':';
        FALLTHROUGH;
        // LCOV_EXCL_STOP
      case required_argument:
        *s++ = ':';
    } // switch
  } // for
  *s = '\0';

  return short_opts;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
