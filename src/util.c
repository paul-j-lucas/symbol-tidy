/*
**      symbol-tidy -- Symbol tidier
**      src/util.c
**
**      Copyright (C) 2013-2026  Paul J. Lucas
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
 * Defines utility data structures, variables, and functions.
 */

// local
#include "pjl_config.h"
#include "util.h"
#include "symbol-tidy.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>                     /* for malloc(), ... */
#include <sysexits.h>

/// @endcond

/**
 * @addtogroup util-group
 * @{
 */

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

char const WS_CHARS[] =           " \n\t\r\f\v";

/// @endcond

////////// extern functions ///////////////////////////////////////////////////

unsigned asprintf_or_exit( char **rv_s, char const *format, ... ) {
  assert( rv_s != NULL );
  assert( format != NULL );

  va_list args;
  va_start( args, format );
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  int const raw_len = vasprintf( rv_s, format, args );
#pragma GCC diagnostic pop
  va_end( args );

  PERROR_EXIT_IF( raw_len < 0, EX_OSERR );
  return STATIC_CAST( unsigned, raw_len );
}

void fatal_error( int status, char const *format, ... ) {
  EPRINTF( "%s: error: ", prog_name );
  va_list args;
  va_start( args, format );
  vfprintf( stderr, format, args );
  va_end( args );
  exit( status );
}

void fputs_escaped( char const *s, FILE *fout ) {
  assert( fout != NULL );

  if ( s == NULL ) {
    fputs( "null", fout );
    return;
  }

  for ( ; *s != '\0'; ++s ) {
    unsigned char c = STATIC_CAST( unsigned char, *s );
    switch ( c ) {
      case '"' : fputs( "\\\"", fout ); break;
      case '\\': fputs( "\\\\", fout ); break;
      case '\a': fputs( "\\a",  fout ); break;
      case '\b': fputs( "\\b",  fout ); break;
      case '\f': fputs( "\\f",  fout ); break;
      case '\n': fputs( "\\n",  fout ); break;
      case '\r': fputs( "\\r",  fout ); break;
      case '\t': fputs( "\\t",  fout ); break;
      case '\v': fputs( "\\v",  fout ); break;

      default:
        if ( likely( c >= 0x20 && c < 0x7F ) )
          fputc( c, fout );
        else
          fprintf( fout, "\\x%02x", c );
        break;
    } // switch
  } // for
}

// LCOV_EXCL_START
void perror_exit( int status ) {
  perror( prog_name );
  exit( status );
}
// LCOV_EXCL_STOP

void* realloc_or_exit( void *p, size_t size ) {
  assert( size > 0 );
  p = realloc( p, size );
  PERROR_EXIT_IF( p == NULL, EX_OSERR );
  return p;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/// @cond DOXYGEN_IGNORE

extern inline bool false_set( bool* );
extern inline char const* plural_s( unsigned long long );
extern inline bool true_or_set( bool* );

/// @endcond

/* vim:set et sw=2 ts=2: */
