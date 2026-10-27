/*
**      symbol-tidy -- Symbol tidier
**      src/options.h
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

#ifndef tidy_options_h
#define tidy_options_h

/**
 * @file
 * Declares types, global variables, and functions for **symbol-tidy**
 * options.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "color.h"

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/**
 * @defgroup options-group Include-Tidy Options
 * Types, global variables, and functions for **symbol-tidy** options.
 * @{
 */

////////// macros /////////////////////////////////////////////////////////////

/**
 * Convenience macro that checks whether being verbose about \a WHAT was
 * requested.
 *
 * @param WHAT What to be verbose about without the `TIDY_VERBOSE_` prefix.
 *
 * @sa opt_verbose
 * @sa tidy_verbose
 */
#define IS_VERBOSE(WHAT)          ((opt_verbose & TIDY_VERBOSE_ ## WHAT) != 0)

#define OPT_COMPILER_DEFAULT      "clang" /**< Default compiler path. */
#define OPT_TEST_ALL              "eh"    /**< All test values. */
#define OPT_VERBOSE_ALL           "acCdfFsSz" /**< All verbose values. */

////////// enums //////////////////////////////////////////////////////////////

/**
 * When to exit with a non-zero status code.
 */
enum tidy_error {
  TIDY_ERROR_IF_UNUSED,                 ///< Non-zero if unused symbols.
  TIDY_ERROR_ALWAYS,                    ///< Always exit with non-zero.
  TIDY_ERROR_NEVER                      ///< Never exit with non-zero.
};

/**
 * Test mode.
 */
enum tidy_test {
  TIDY_TEST_NONE        = 0,            ///< Not being tested.
  TIDY_TEST_NO_ETC_XDG  = 1 << 0,       ///< Don't read files under `/etc/xdg`.
  TIDY_TEST_NO_HOME     = 1 << 1,       ///< Don't read files under `$HOME`.
};

/**
 * Verbose mode.
 *
 * @note If this is updated, ensure #OPT_VERBOSE_ALL matches.
 *
 * @sa #IS_VERBOSE()
 * @sa opt_verbose
 * @sa opt_verbose_parse()
 */
enum tidy_verbose {
  TIDY_VERBOSE_NONE,                            ///< Don't be verbose.
  TIDY_VERBOSE_ARGS                 = 1 << 0,   ///< Command-line arguments.
  TIDY_VERBOSE_CONFIG_FILES         = 1 << 1,   ///< Configuration files.
  TIDY_VERBOSE_CONFIG_SYMBOLS       = 1 << 2,   ///< Configuration symbols.
  TIDY_VERBOSE_CURSORS              = 1 << 3,   ///< Libclang cursors.
  TIDY_VERBOSE_DIRECTORY            = 1 << 4,   ///< Changing directory.
  TIDY_VERBOSE_SRC_FILE_VIOLATIONS  = 1 << 5,   ///< Source file in violation.
  TIDY_VERBOSE_SRC_FILE_ALWAYS      = 1 << 6,   ///< Always source file.
  TIDY_VERBOSE_STATISTICS           = 1 << 7,   ///< Print statistics?
  TIDY_VERBOSE_SYMBOLS              = 1 << 8,   ///< Symbols referenced.
};

////////// typedefs ///////////////////////////////////////////////////////////

typedef enum tidy_error   tidy_error;
typedef enum tidy_test    tidy_test;
typedef enum tidy_verbose tidy_verbose;

////////// extern option variables ////////////////////////////////////////////

extern color_when     opt_color_when;     ///< When to colorize.
extern bool           opt_config_layers;  ///< Do configuration file layering?
extern char const    *opt_config_path;    ///< Configuration file path.
extern bool           opt_debug;          ///< Print debugging output?
extern tidy_error     opt_error;          ///< When to exit with non-zero.

extern
enum CXLanguageKind   opt_lang;           ///< Explicitly specified language.

extern tidy_test      opt_test;           ///< Is **symbol-tidy** under test?
extern tidy_verbose   opt_verbose;        ///< Print verbose output?

////////// extern functions ///////////////////////////////////////////////////

/**
 * Parses when to colorize.
 *
 * @param s The string to parse.
 * @return Returns `true` only if \a s was parsed successfully.
 */
NODISCARD
bool opt_color_parse( char const *s );

/**
 * Parses when to exit with a non-zero exit status.
 *
 * @param s The error to parse.
 * @return Returns `true` only if \a s parsed successfully.
 */
NODISCARD
bool opt_error_parse( char const *s );

/**
 * Parses the value of the test environment variable.
 *
 * @param test_format
 * @parblock
 * The NULL-terminated **symbol-tidy** test format string (case sensitive) to
 * parse.  Valid formats are:
 *
 * Format | Meaning
 * -------|-----------------------------------------------------------------
 * `e`    | Don't read files under `/etc/xdg/symbol-tidy` by default.
 * `h`    | Don't read files under the user's home directory by default.
 *
 * Multiple formats may be given, one immediately after the other, e.g., `eh`.
 * Alternatively, `*` may be given to mean "all" or either the empty string or
 * `-` may be given to mean "none."
 * @endparblock
 * @return Returns `true` only if \a test_format parsed successfully.
 */
NODISCARD
bool opt_test_parse( char const *test_format );

/**
 * Parses the value of the **symbol-tidy** verbose option.
 *
 * @param verbose_format
 * @parblock
 * The null-terminated **symbol-tidy** verbose format string to parse.
 * Valid formats are:
 *
 * Format | Be verbose about ...
 * -------|-----------------------------------------------------------------
 * `a`    | Command-line arguments.
 * `c`    | Configuration files read or attempted.
 * `C`    | Libclang cursors.
 * `d`    | Directory changing to from the `--directory` or `-d` option.
 * `f`    | Name of source-file only if in violation.
 * `F`    | Name of source-file.
 * `S`    | Configuration file symbols.
 * `z`    | Statistics.
 *
 * Multiple formats may be given, one immediately after the other, e.g., `ac`.
 * Alternatively, `*` may be given to mean "all" or either the empty string or
 * `-` may be given to mean "none."
 * @endparblock
 * @return Returns `true` only if \a verbose_format parsed successfully.
 */
NODISCARD
bool opt_verbose_parse( char const *verbose_format );

/**
 * If \a *pformat is:
 *
 *  + `"*"`: sets \a *pformat to \a all_value.
 *  + `"-"`: sets \a *pformat to `""` (the empty string).
 *
 * Otherwise does nothing.
 *
 * @param pformat A pointer to the format string to possibly set.
 * @param all_value The "all" value for when \a *pformat is `"*"`.
 */
void option_str_set_all_or_none( char const **pformat, char const *all_value );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_options_h */
/* vim:set et sw=2 ts=2: */
