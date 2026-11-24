/*
**      symbol-tidy -- Symbol tidier
**      src/cli_util.h
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

#ifndef tidy_cli_util_h
#define tidy_cli_util_h

/**
 * @file
 * Defines functions for command-line utilities.
 */

// local
#include "pjl_config.h"                 /* must go first */

/// @cond DOXYGEN_IGNORE

// standard
#include <getopt.h>

/// @endcond

/**
 * @defgroup cli-util-group Command Line Utilities
 * Functions for command-line utilities
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Checks whether the \a argv[\a *pargi] is a long option \a long_opt having an
 * argument: if so, returns that argument.
 *
 * @param argc The command-line argument count from `main()`.
 * @param argv The command-line argument values from `main()`.
 * @param long_opt The long option (without the `--`).
 * @param pargi A pointer to the current argument index variable.
 * @return Returns the value of the argument or NULL if \a argv[\a *pargi] is
 * not \a long_opt.
 *
 * @sa get_short_opt_value()
 */
NODISCARD
char const* get_long_opt_value( int argc, char const *const argv[],
                                char const *long_opt, int *pargi );

/**
 * Formats an option as `--%%s/-%%c` where `%%s` is the long option and `%%c`
 * is the short option.
 *
 * @param short_opt The short option (along with its corresponding long option)
 * to format.
 * @return Returns the formatted string.
 *
 * @warning The pointer returned is to one of a small number of static buffers,
 * so you can't do something like call this more than twice in the same
 * `printf()` statement.
 */
NODISCARD
char const* get_opt_format( int short_opt );

/**
 * Gets the `option` corresponding to \a long_opt.
 *
 * @param long_opt The long option to get the option for.
 * @return Returns the corresponding `option` or NULL if not found.
 *
 * @sa get_option_short()
 */
NODISCARD
struct option const* get_option_long( char const *long_opt );

/**
 * Gets the `option` corresponding to \a short_opt.
 *
 * @param short_opt The short option to get the option for.
 * @return Returns the corresponding `option` or NULL if not found.
 *
 * @sa get_option_long()
 */
NODISCARD
struct option const* get_option_short( int short_opt );

/**
 * Checks whether the \a argv[\a *pargi] is a short option \a short_opt having
 * an argument: if so, returns that argument.
 *
 * @param argc The command-line argument count from `main()`.
 * @param argv The command-line argument values from `main()`.
 * @param short_opt The short option character.
 * @param pargi A pointer to the current argument index variable.
 * @return Returns the value of the argument or NULL if \a argv[\a *pargi] is
 * not \a short_opt.
 *
 * @sa get_long_opt_value()
 */
NODISCARD
char const* get_short_opt_value( int argc, char const *const argv[],
                                 int short_opt, int *pargi );

/**
 * Makes the `optstring` (short option) equivalent of \a opts for the third
 * argument of `getopt_long()`.
 *
 * @return Returns the `optstring` for the third argument of `getopt_long()`.
 *
 * @note The caller is responsible for freeing the returned string.
 */
NODISCARD
char* make_short_opts( void );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_cli_util_h */
/* vim:set et sw=2 ts=2: */
