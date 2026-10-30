/*
**      PJL Library
**      src/cxx_util.c
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
 * Defines C++ utility functions.
 */

// local
#include "pjl_config.h"
#include "cxx_util.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <ctype.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup cxx-util-group
 * @{
 */

////////// local functions ////////////////////////////////////////////////////

/**
 * Gets whether \a c is a valid C identifier character.
 *
 * @param c The character to check.
 * @return Returns `true` only if \a is a valid C identifier character.
 */
static inline bool is_ident( char c ) {
  return isalnum( c ) || c == '_';
}

////////// extern functions ///////////////////////////////////////////////////

char const* cxx_name_check( char const *name ) {
  assert( name != NULL );

  if ( !is_ident( *name ) )
    return name;

  while ( *name != '\0' ) {
    if ( is_ident( *name ) ||
         (*name == ':' && *++name == ':' && is_ident( *++name )) ) {
      ++name;
    }
    else {
      return name;
    }
  } // while

  return NULL;
}

char const* cxx_unqual_name( char const *name ) {
  assert( name != NULL );

  char const *unqual_name = name;

  while ( *name != '\0' ) {
    if ( name[0] != ':' ) {
      ++name;
    }
    else if ( name[1] == ':' ) {
      name += 2;
      unqual_name = name;
    }
    else
      ++name;
  } // while

  return unqual_name;
}

int cxx_unqual_strcmp( char const *i_name, char const *j_name ) {
  assert( i_name != NULL );
  assert( j_name != NULL );

  char const *const i_unqual_name = cxx_unqual_name( i_name );
  char const *const j_unqual_name = cxx_unqual_name( j_name );
  return strcmp( i_unqual_name, j_unqual_name );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
