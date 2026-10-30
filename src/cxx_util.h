/*
**      PJL Library
**      src/cxx_util.h
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

#ifndef cxx_util_h
#define cxx_util_h

/**
 * @file
 * Declares C++ utility functions.
 */

// local
#include "pjl_config.h"

/**
 * @defgroup cxx-util-group C++ Utility Functions
 * C++ utility functions.
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Checks if \a name is a valid C name or a valid C++ qualified name.
 *
 * @param name The name to check.
 * @return Returns NULL only if \a name is valid or a pointer to within \a name
 * of the first invalid character.
 */
NODISCARD
char const* cxx_name_check( char const *name );

/**
 * Gets the unqualified name of a C++ scoped name.
 *
 * @param name The C++ scoped name.
 * @return Returns a pointer to within \a name of its unqualified name or \a
 * name if it contains no scope resolution operators (`::`).
 *
 * @warning This function assumes \a name is a valid C++ scoped name.
 *
 * @sa cxx_name_check()
 */
NODISCARD
char const* cxx_unqual_name( char const *name );

/**
 * Compares two C++ unqualified names.
 *
 * @param i_name The fisrt name.
 * @param j_name The second name.
 * @return Returns a number less than 0, 0, or greater than 0 if the
 * unqualified name of \a i_name is less than, equal to, or greater than the
 * unqualified name of \a j_name, respectively.
 *
 * @warning This function assumes \a i_name and \a j_name are both valid C++
 * scoped names.
 *
 * @sa cxx_name_check()
 */
NODISCARD
int cxx_unqual_strcmp( char const *i_name, char const *j_name );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* cxx_util_h */
/* vim:set et sw=2 ts=2: */
