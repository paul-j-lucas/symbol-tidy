/*
**      symbol-tidy -- Symbol tidier
**      src/cli_options.c
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
 * Defines macros and functions for command-line options.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "cli_options.h"
#include "array.h"
#include "lang.h"
#include "options.h"
#include "path_util.h"
#include "print.h"
#include "source.h"
#include "str_util.h"
#include "strbuf.h"
#include "symbol-tidy.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <ctype.h>                      /* for isalnum(), isprint() */
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>                     /* for exit() */
#include <string.h>                     /* for str...() */
#include <sys/wait.h>
#include <sysexits.h>
#include <unistd.h>                     /* for chdir(2) */

/// @endcond

/**
 * @addtogroup cli-options-group
 * @{
 */

////////// macros /////////////////////////////////////////////////////////////

/**
 * When given the `-v` (verbose) option, both `clang` and `gcc` emit this line
 * followed by a list of default system include search paths.
 *
 * @sa argv_add_include_paths()
 * @sa #COMPILER_SEARCH_END
 */
#define COMPILER_SEARCH_START  "#include <...> search starts here:"

/**
 * When given the `-v` (verbose) option, both `clang` and `gcc` emit this line
 * following a list of default system include search paths.
 *
 * @sa argv_add_include_paths()
 * @sa #COMPILER_SEARCH_START
 */
#define COMPILER_SEARCH_END    "End of search list."

/**
 * Convenience macro for iterating over all command-line options.
 *
 * @param VAR The `struct option` loop variable.
 */
#define FOREACH_CLI_OPTION(VAR, OPTIONS) \
  for ( struct option const *VAR = (OPTIONS); (VAR)->name != NULL; ++(VAR) )

////////// local constants ////////////////////////////////////////////////////

/**
 * Shared partial error message for an invalid `-Xtidy` option.
 */
static char const INVALID_XTIDY_OPTION[] =
  "invalid -Xtidy option; use --help or -h for help";

/**
 * Command-line options.
 *
 * @sa OPTIONS_HELP
 */
static struct option const OPTIONS[] = {
  { "color",            required_argument,  NULL, COPT(COLOR)             },
  { "compiler",         required_argument,  NULL, COPT(COMPILER)          },
  { "config",           required_argument,  NULL, COPT(CONFIG)            },
  { "debug",            no_argument,        NULL, COPT(DEBUG)             },
  { "directory",        required_argument,  NULL, COPT(DIRECTORY)         },
  { "error",            required_argument,  NULL, COPT(ERROR)             },
  { "help",             no_argument,        NULL, COPT(HELP)              },
  { "no-config-layers", no_argument,        NULL, COPT(NO_CONFIG_LAYERS)  },
  { "verbose",          required_argument,  NULL, COPT(VERBOSE)           },
  { "version",          no_argument,        NULL, COPT(VERSION)           },
  { NULL,               0,                  NULL, 0                       }
};

/**
 * Command-line options help.
 *
 * @note It is indexed by short option characters.
 *
 * @sa get_opt_help()
 * @sa OPTIONS
 */
static char const *const OPTIONS_HELP[] = {
  [ COPT(COLOR) ] = "When to colorize output; default=\"not_file\"",
  [ COPT(COMPILER) ] = "Path of compiler to use or \"none\"; default=\"" OPT_COMPILER_DEFAULT "\"",
  [ COPT(CONFIG) ] = "Configuration file path",
  [ COPT(DEBUG) ] = "Print " PACKAGE " debug output",
  [ COPT(DIRECTORY) ] = "Change directory before tidying",
  [ COPT(ERROR) ] = "When to exit with a non-zero status",
  [ COPT(HELP) ] = "Print this help and exit",
  [ COPT(NO_CONFIG_LAYERS) ] = "Don't do configuration file layering",
  [ COPT(VERBOSE) ] = "Print verbose output",
  [ COPT(VERSION) ] = "Print version and exit",
};

////////// local variables ////////////////////////////////////////////////////

static bool is_opt_set[ 128 ];          ///< Table of options that were set.

/**
 * @anchor ptu_argv_global
 * Each array is a list of command-line arguments that will eventually be
 * passed to `clang_parseTranslationUnit2` where:
 *
 *  + `[0]` is for C source files.
 *  + `[1]` is for C++ source files.
 *
 * @remarks Each lannguage needs its own list of arguments since the set of
 * include paths among them may be different.
 *
 * @note Unlike an `argv[0]` from `main()` that is conventionally the program's
 * name, each array's zeroth element is its first argument.
 *
 * @sa sources_init_argv()
 */
static array_t ptu_argv[2] = {
  ARRAY_INIT( sizeof(char*) ),
  ARRAY_INIT( sizeof(char*) ),
};

////////// local functions ////////////////////////////////////////////////////

NODISCARD
static char const*          get_opt_format( int );

NODISCARD
static struct option const* get_option_short( int );

/////////// local functions ///////////////////////////////////////////////////

/**
 * Gets the index within \a argv to insert include-path options.
 *
 * @param argv An array of command-line options.
 * @return Returns said index.
 */
NODISCARD
static unsigned argv_get_include_index( array_t *argv ) {
  assert( argv != NULL );

  size_t const argc = argv->len;
  unsigned first_arg_after = 0;
  char const *opt = NULL;

  for ( size_t i = 0; i < argv->len; ++i ) {
    char *const argv_i = *(char**)array_at_nc( argv, i );

    if ( STRNCMPLIT( argv_i, "-I" ) == 0 ) {
      if ( argv_i[2] == '\0' ) {       // -I <dir>
        if ( ++i >= argc || argv_i[0] == '-' ) {
          opt = "I";
          goto short_opt_requires_argument;
        }
      }
      first_arg_after = STATIC_CAST( unsigned, i ) + 1;
      continue;
    }

    static char const *const LONG_INCLUDE_OPTIONS[] = {
      "--include-directory-after",
      "--include-directory",
      "-isystem-after",
      "-cxx-isystem",
      "-idirafter",
      "-isystem",
      "-iquote",
    };

    FOREACH_ARRAY_ELEMENT( char const*, popt, LONG_INCLUDE_OPTIONS ) {
      size_t const opt_len = strlen( *popt );
      if ( strncmp( argv_i, *popt, opt_len ) == 0 ) {
        char const *const argv_rest = argv_i + opt_len;
        char const *dir_arg;

        switch ( argv_rest[0] ) {
          case '\0':                    // --<long_opt> <dir>
            if ( ++i >= argc )
              fatal_error( EX_USAGE, "\"%s\" requires an argument\n", *popt );
            dir_arg = argv_i;
            break;
          case '=':                     // --<long_opt>=<dir>
            dir_arg = argv_rest + 1;
            break;
          default:                      // --<long_opt><dir>
            dir_arg = argv_rest;
            break;
        } // switch

        if ( dir_arg[0] == '\0' || dir_arg[0] == '-' ) {
          opt = *popt;
          goto long_opt_requires_argument;
        }
        first_arg_after = STATIC_CAST( unsigned, i ) + 1;
      }
    } // for
  } // for

  return first_arg_after;

long_opt_requires_argument:
  assert( opt != NULL );
  if ( opt[0] == '-' )
    fatal_error( EX_USAGE, "\"%s\" requires an argument\n", opt );
  fatal_error( EX_USAGE, "\"--%s\" requires an argument\n", opt );

short_opt_requires_argument:
  assert(  opt != NULL );
  assert( isalnum( *opt ) );
  fatal_error( EX_USAGE, "\"-%c\" requires an argument\n", *opt );
}

/**
 * Calls the C or C++ compiler, either **clang**(1) or **gcc**(1), and parses
 * its verbose output to get the list of include search paths that are inserted
 * into \a argv.
 *
 * @param argv A pointer to command-line argument values. An argument of the
 * form <tt>-isystem</tt><i>path</i> is inserted for each search path.
 * @param compiler_path The path of the C or C++ compiler to use.
 * @param lang The language to get the search paths for.
 */
static void argv_add_include_paths( array_t *argv, char const *compiler_path,
                                    enum CXLanguageKind lang ) {
  assert( argv != NULL );
  assert( compiler_path != NULL );
  assert( lang != CXLanguage_Invalid );

  static char const COMMAND[] =
    "%s"          // compiler path
    " -E"         // run only the preprocessor stage
    " -v"         // show verbose output
    " -x%s"       // set language: c or c++
    " -"          // read from stdin
    " </dev/null" // set stdin to /dev/null
    " 2>&1";      // redirect stderr to stdout

  char *command = NULL;
  asprintf_or_exit(
    &command, COMMAND, compiler_path,
    lang == CXLanguage_CPlusPlus ? "c++" : "c"
  );

  FILE *const fcompiler = popen( command, "r" );
  free( command );
  if ( fcompiler == NULL )
    goto error;                         // LCOV_EXCL_LINE

  bool      found_search_start = false;
  bool      found_search_end = false;
  char     *line_buf = NULL;
  size_t    line_cap = 0;

  while ( getline( &line_buf, &line_cap, fcompiler ) != -1 ) {
    if ( strcmp( line_buf, COMPILER_SEARCH_START "\n" ) == 0 ) {
      found_search_start = true;
      break;
    }
  } // while
  if ( !found_search_start )
    goto done;

  size_t  isystem_argi = argv_get_include_index( argv );
  ssize_t line_len;

  while ( (line_len = getline( &line_buf, &line_cap, fcompiler )) != -1 ) {
    if ( strcmp( line_buf, COMPILER_SEARCH_END "\n" ) == 0 ) {
      found_search_end = true;
      break;
    }

#ifdef __APPLE__
    // On macOS, the C compiler's include search paths include frameworks
    // directories denoted by having paths followed by " (framework
    // directory)".  These don't contain .h files directly, so there's no point
    // in including them.
    static char FRAMEWORK_DIR[] = "(framework directory)\n";
    if ( str_ends_with( line_buf, FRAMEWORK_DIR, STRLITLEN( FRAMEWORK_DIR ) ) )
      continue;
#endif /* __APPLE__ */

    char const *const include_path =
      strn_trim( line_buf, STATIC_CAST( size_t, line_len ) );
    if ( unlikely( path_is_relative( include_path ) ) )
      continue;                         // LCOV_EXCL_LINE

    char *new_arg = NULL;
    asprintf_or_exit( &new_arg, "-isystem%s", include_path );
    *(char**)array_insert( argv, isystem_argi++, 1 ) = new_arg;
  } // while

done:
  free( line_buf );

  // To prevent a SIGPIPE, drain the pipe by reading any remaining output.
  char drain_buf[ BUFSIZ ];
  while ( fread( drain_buf, 1, sizeof drain_buf, fcompiler ) > 0 )
    ;

  int status = pclose( fcompiler );
  if ( status == -1 )
    goto error;                         // LCOV_EXCL_LINE

  if ( WIFEXITED( status ) ) {
    status = WEXITSTATUS( status );
    switch ( status ) {
      case 0:
        if ( !found_search_start ) {
          print_warning(
            "\"" COMPILER_SEARCH_START "\" not found in \"%s\" output\n",
            compiler_path
          );
        }
        else if ( !found_search_end ) {
          print_warning(
            "\"" COMPILER_SEARCH_END "\" not found in \"%s\" output\n",
            compiler_path
          );
        }
        return;
      case 127:                         // POSIX shell "file not found"
        errno = ENOENT;
        goto error;
      default:
        // LCOV_EXCL_START
        fatal_error( EX_UNAVAILABLE,
          "\"%s\": invocation failed with status %d\n", compiler_path, status
        );
        // LCOV_EXCL_STOP
    } // switch
  }

  // LCOV_EXCL_START
  if ( WIFSIGNALED( status ) ) {
    int const sig = WTERMSIG( status );
    fatal_error( EX_UNAVAILABLE,
      "shell executing \"%s\" killed by signal %d: %s\n",
      compiler_path, sig, strsignal( sig )
    );
  }
  else {
    fatal_error( EX_UNAVAILABLE,
      "shell executing \"%s\" terminated abnormally\n",
      compiler_path
    );
  }
  // LCOV_EXCL_STOP

error:
  fatal_error( EX_UNAVAILABLE,
    "\"%s\": invocation failed: %s\n", compiler_path, STRERROR()
  );
}

/**
 * If \a short_opt was given, checks that _only_ it was given and, if not,
 * prints an error message and exits; if \a short_opt was not given, does
 * nothing.
 *
 * @param short_opt The option to check for.
 */
static void check_opt_exclusive( int short_opt ) {
  if ( !option_is_set( short_opt ) )
    return;
  for ( int curr_opt = '0'; curr_opt < 128; ++curr_opt ) {
    if ( curr_opt == short_opt )
      continue;
    if ( option_is_set( curr_opt ) ) {
      fatal_error( EX_USAGE,
        "%s can be given only by itself\n",
        get_opt_format( short_opt )
      );
    }
  } // for
}

/**
 * Checks option combinations for semantic errors.
 */
static void check_options( void ) {
  check_opt_exclusive( COPT(HELP) );
  check_opt_exclusive( COPT(VERSION) );
}

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
static char const* get_long_opt_value( int argc, char const *const argv[],
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
    if ( value != NULL && value[0] != '\0' && value[0] != '-' )
      return value;
  }

  fatal_error( EX_USAGE, "\"--%s\" requires an argument\n", long_opt );
}

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
static char const* get_opt_format( int short_opt ) {
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

/**
 * Gets the help message for \a short_opt.
 *
 * @param short_opt The option to get the help for.
 * @return Returns said help message.
 */
NODISCARD
static char const* get_opt_help( int short_opt ) {
  assert( short_opt > 0 );
  assert( STATIC_CAST( size_t, short_opt ) < ARRAY_SIZE( OPTIONS_HELP ) );
  char const *const help = OPTIONS_HELP[ short_opt ];
  assert( help != NULL );
  return help;
}

/**
 * Gets the `option` corresponding to \a long_opt.
 *
 * @param long_opt The long option to get the option for.
 * @return Returns the corresponding `option` or NULL if not found.
 *
 * @sa get_option_short()
 */
NODISCARD
static struct option const* get_option_long( char const *long_opt ) {
  assert( long_opt != NULL );
  char const *const equal = strchr( long_opt, '=' );
  size_t const n = equal != NULL ?
    STATIC_CAST( size_t, equal - long_opt ) : strlen( long_opt );

  FOREACH_CLI_OPTION( option, OPTIONS ) {
    if ( strncmp( option->name, long_opt, n ) == 0 )
      return option;
  } // for
  return NULL;
}

/**
 * Gets the `option` corresponding to \a short_opt.
 *
 * @param short_opt The short option to get the option for.
 * @return Returns the corresponding `option` or NULL if not found.
 *
 * @sa get_option_long()
 */
NODISCARD
static struct option const* get_option_short( int short_opt ) {
  assert( short_opt > 0 && short_opt < 128 );
  FOREACH_CLI_OPTION( option, OPTIONS ) {
    if ( option->val == short_opt )
      return option;
  } // for
  return NULL;
}

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
static char const* get_short_opt_value( int argc, char const *const argv[],
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
    if ( value != NULL && value[0] != '\0' && value[0] != '-' )
      return value;
  }

  fatal_error( EX_USAGE, "\"-%c\" requires an argument\n", short_opt );
}

/**
 * Creates a tidy_source object for each source file to be tidied.
 *
 * @param argc The command-line argument count from `main()`.
 * @param argv The command-line argument values from `main()`.
 * @param rv_sources An array to receive the tidy_source files to be tidied.
 * @return Returns \a argc minus all the source file and language options
 * removed.
 */
static int get_sources( int argc, char const *const argv[],
                        array_t *rv_sources ) {
  assert( argc > 0 );
  assert( argv != NULL );
  assert( rv_sources != NULL );

  struct lang_map {
    char const         *lang_name;
    enum CXLanguageKind lang_kind;
  };
  typedef struct lang_map lang_map;

  static lang_map const LANG_MAP[] = {
    { "c",    CXLanguage_C          },
    { "c++",  CXLanguage_CPlusPlus  },
    { "none", CXLanguage_Invalid    }
  };

  int argi = 0;

  //
  // Find the first -x/--language option: if found, it marks the start of the
  // source files in argv.
  //
  for ( int i = 1; i < argc; ++i ) {
    int i_copy = i;
    char const *lang_value = get_short_opt_value( argc, argv, 'x', &i_copy );
    if ( lang_value == NULL )
      lang_value = get_long_opt_value( argc, argv, "language", &i_copy );
    if ( lang_value == NULL )
      continue;
    argi = i;
  } // for

  //
  // If there's no -x/--language option, iterate backwards looking for the last
  // option or argument without a filename extension: the argument after it
  // marks the start of the source files in argv.
  //
  if ( argi == 0 ) {
    for ( int i = argc - 1; i >= 1; --i ) {
      char const *const argv_i = argv[i];
      if ( argv_i[0] == '-' )
        break;
      char const *const ext = path_ext( argv_i );
      if ( ext == NULL )
        break;
      enum CXLanguageKind lang = lang_parse_ext( ext );
      if ( lang == CXLanguage_Invalid )
        break;
      argi = i;
    } // for
  }

  if ( argi == 0 )
    return 0;

  //
  // Finally, iterate forwards starting at argi constructing tidy_source
  // objects.
  //
  enum CXLanguageKind x_lang = CXLanguage_Invalid;

  for ( int i = argi; i < argc; ++i ) {
    char const *lang_value = get_short_opt_value( argc, argv, 'x', &i );
    if ( lang_value == NULL )
      lang_value = get_long_opt_value( argc, argv, "language", &i );
    if ( lang_value != NULL ) {
      bool found_lang = false;
      FOREACH_ARRAY_ELEMENT( lang_map, map, LANG_MAP ) {
        if ( strcmp( lang_value, map->lang_name ) == 0 ) {
          x_lang = map->lang_kind;
          found_lang = true;
          break;
        }
      } // for
      if ( !found_lang ) {
        fatal_error( EX_USAGE,
          "\"%s\": invalid value for -x/--language;"
          " must be one of \"c\", \"c++\", or \"none\"\n",
          lang_value
        );
      }
      continue;
    }

    enum CXLanguageKind lang = CXLanguage_Invalid;
    if ( x_lang != CXLanguage_Invalid ) {
      lang = x_lang;
    }
    else {
      char const *const ext = path_ext( argv[i] );
      if ( ext == NULL ) {
        fatal_error( EX_USAGE,
          "\"%s\": can't deduce language without extension"
          "; use -x/--language\n",
          argv[i]
        );
      }
      lang = lang_parse_ext( ext );
      if ( lang == CXLanguage_Invalid ) {
        fatal_error( EX_USAGE,
          "\"%s\": can't deduce language from extension"
          "; use -x/--language\n",
          argv[i]
        );
      }
    }

    tidy_source source = {
      .argv_path = path_no_dot_slash( argv[i] ),
      .lang = lang
    };
    *(tidy_source*)array_push_back( rv_sources ) = source;
  } // for

  return argi;
}

/**
 * Checks whether the \a argv[\a *pargi] is `-Xtidy` and followed by a
 * subsequent option.
 *
 * @param argc The command-line argument count from `main()`.
 * @param argv The command-line argument values from `main()`.
 * @param pargi A pointer to the current argument index variable.  It's
 * incremented only if returning `true`.
 * @return Returns `true` only if `argv[*pargi]` is `-Xtidy` and is followed by
 * a subsequent option.
 */
NODISCARD
static bool is_Xtidy_opt( int argc, char const *const argv[], int *pargi ) {
  assert(   argc > 0 );
  assert(   argv != NULL );
  assert(  pargi != NULL );
  assert( *pargi < argc );

  if ( strcmp( argv[ *pargi ], "-Xtidy" ) != 0 )
    return false;
  if ( ++*pargi >= argc )               // -Xtidy (nothing)
    goto error;
  if ( argv[ *pargi ][0] != '-' )       // -Xtidy A (non-option)
    goto error;
  if ( isalnum( argv[ *pargi ][1] ) )   // -Xtidy -A (short option)
    return true;
  if ( argv[ *pargi ][1] == '-' && isalnum( argv[ *pargi ][2] ) )
    return true;                        // -Xtidy --A (long option)

error:
  fatal_error( EX_USAGE, "-Xtidy requires subsequent option\n" );
}

/**
 * Makes the `optstring` (short option) equivalent of \a opts for the third
 * argument of `getopt_long()`.
 *
 * @param options An array of options to make the short option string from.
 * Its last element must be all zeros.
 * @return Returns the `optstring` for the third argument of `getopt_long()`.
 *
 * @note The caller is responsible for freeing the returned string.
 */
NODISCARD
static char* make_short_opts( struct option const options[static const 2] ) {
  // pre-flight to calculate string length
  size_t len = 1 /* for leading ':' */;
  FOREACH_CLI_OPTION( option, options ) {
    assert( option->has_arg >= 0 && option->has_arg <= 2 );
    len += 1 + STATIC_CAST( unsigned, option->has_arg );
  } // for

  char *const short_opts = MALLOC( char, len + 1/*\0*/ );
  char *s = short_opts;

  *s++ = ':';                           // return missing argument as ':'
  FOREACH_CLI_OPTION( option, options ) {
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

/**
 * Moves **symbol-tidy** specific command-line options to a separate array.
 *
 * @remarks
 * @parblock
 * Command-line options of the form:
 *
 *  + <tt>-Xtidy</tt> _option_ have <tt>-Xtidy</tt> removed and _option_ moved
 *    to \a *ptidy_argv.
 *  + <tt>--help</tt>, <tt>-h</tt>, <tt>--version</tt>, or <tt>-v</tt> are
 *    moved to \a *ptidy_argv.
 *
 * All other options and arguments are left as-is in \a argv.  Examples:
 *
 *      symbol-tidy --help
 *
 *          argc = 1
 *          argv[0] = "symbol-tidy"
 *          argv[1] = NULL
 *
 *          tidy_argc = 2
 *          tidy_argv[0] = "symbol-tidy"
 *          tidy_argv[1] = "--help"
 *          tidy_argv[2] = NULL
 *
 *      symbol-tidy -Xtidy --foo -DNDEBUG -I/opt/local/include bar.c
 *
 *          argc = 4
 *          argv[0] = "symbol-tidy"
 *          argv[1] = "-DNDEBUG"
 *          argv[2] = "-I/opt/local/include"
 *          argv[3] = "bar.c"
 *          argv[4] = NULL
 *
 *          tidy_argc = 3
 *          tidy_argv[0] = "symbol-tidy"
 *          tidy_argv[1] = "--foo"
 *          tidy_argv[2] = NULL
 * @endparblock
 *
 * @param pargc A pointer to the command-line argument count from `main()`.  On
 * return, it's set to the number of remaining options for libclang.
 * @param argv The command-line argument values from `main()`.
 * @param ptidy_argc A pointer to receive the argument count for **symbol-
 * tidy** specific options.
 * @param ptidy_argv A pointer to receive the argument values for **symbol-
 * tidy** specific options.  The caller is responsible for free'ing the array
 * of pointers to strings, but _not_ the strings themselves since they point at
 * strings from \a argv.
 */
static void move_tidy_args( int *pargc, char const *argv[],
                            int *ptidy_argc, char const **ptidy_argv[] ) {
  assert(  pargc != NULL );
  assert( *pargc > 0 );
  assert(   argv != NULL );
  assert(  ptidy_argc != NULL );
  assert(  ptidy_argv != NULL );

  int const argc = *pargc;
  int new_argc = 1, tidy_argc = 1;
  char const *opt = NULL;

  char const **const tidy_argv =
    MALLOC( char*, STATIC_CAST( size_t, argc ) + 1/*NULL*/ );
  tidy_argv[0] = argv[0];

  for ( int i = 1; i < argc; ++i ) {

    if ( is_Xtidy_opt( argc, argv, &i ) ) {
      tidy_argv[ tidy_argc++ ] = argv[i];
      if ( isalnum( argv[i][1] ) ) {            // short opt
        for ( opt = &argv[i][1]; *opt != '\0'; ++opt ) {
          struct option const *const option = get_option_short( *opt );
          if ( option == NULL )
            fatal_error( EX_USAGE, "'%c': %s\n", *opt, INVALID_XTIDY_OPTION );
          if ( option->has_arg == required_argument ) {
            if ( opt[1] != '\0' )               // -<opt><arg>
              break;
            if ( ++i >= argc || argv[i][0] == '-' )
              goto short_opt_requires_argument;
            tidy_argv[ tidy_argc++ ] = argv[i]; // -<opt> <arg>
            break;
          }
        } // for
      }
      else {                                    // must be long opt
        assert( argv[i][1] == '-' );
        opt = &argv[i][2];
        struct option const *const option = get_option_long( opt );
        if ( option == NULL )
          fatal_error( EX_USAGE, "\"%s\": %s\n", opt, INVALID_XTIDY_OPTION );
        if ( option->has_arg == required_argument ) {
          char *const equal = strchr( opt, '=' );
          if ( equal != NULL ) {                // --<long_opt>=<value>
            if ( equal[1] == '\0' ) {
              *equal = '\0';
              goto long_opt_requires_argument;
            }
          }
          else {                                // --<long_opt> <value>
            if ( ++i >= argc || argv[i][0] == '-' )
              goto long_opt_requires_argument;
            tidy_argv[ tidy_argc++ ] = argv[i];
          }
        }
      }
      continue;
    }

    static char const *const OTHER_OPTIONS[] = {
      "--help",     "-h",
      "--version",  "-v",
    };

    FOREACH_ARRAY_ELEMENT( char const*, popt, OTHER_OPTIONS ) {
      size_t const opt_len = strlen( *popt );
      if ( strncmp( argv[i], *popt, opt_len ) == 0 ) {
        tidy_argv[ tidy_argc++ ] = argv[i];
        goto next_argv;
      }
    } // for

    argv[ new_argc++ ] = argv[i];

next_argv:;
  } // for

  argv[ new_argc ] = tidy_argv[ tidy_argc ] = NULL;

  *pargc = new_argc;
  *ptidy_argc = tidy_argc;
  *ptidy_argv = tidy_argv;
  return;

long_opt_requires_argument:
  assert( opt != NULL );
  if ( opt[0] == '-' )
    fatal_error( EX_USAGE, "\"%s\" requires an argument\n", opt );
  fatal_error( EX_USAGE, "\"--%s\" requires an argument\n", opt );

short_opt_requires_argument:
  assert(  opt != NULL );
  assert( isalnum( *opt ) );
  fatal_error( EX_USAGE, "\"-%c\" requires an argument\n", *opt );
}

/**
 * Prints the usage message to standard error and exits.
 *
 * @param status The status to exit with.  If it is `EX_OK`, prints to standard
 * output; otherwise prints to standard error.
 */
_Noreturn
static void print_usage( int status ) {
  // pre-flight to calculate longest long option length
  size_t longest_opt_len = 0;
  FOREACH_CLI_OPTION( option, OPTIONS ) {
    size_t long_opt_len = strlen( option->name );
    switch ( option->has_arg ) {
      case no_argument:
        break;
      case optional_argument:
        // LCOV_EXCL_START: tidy doesn't currently have any optional arguments.
        long_opt_len += STRLITLEN( "[=ARG]" );
        break;
        // LCOV_EXCL_STOP
      case required_argument:
        long_opt_len += STRLITLEN( "=ARG" );
        break;
    } // switch
    if ( long_opt_len > longest_opt_len )
      longest_opt_len = long_opt_len;
  } // for

  FILE *const fout = status == EX_OK ? stdout : stderr;

  fprintf( fout,
    "usage: %s [-Xtidy tidy-option]... [compiler-option]... source-file...\n"
    "       %s other-option\n"
    "\n"
    "tidy options:\n"
    , prog_name, prog_name
  );

  FOREACH_CLI_OPTION( option, OPTIONS ) {
    switch ( option->val ) {
      case COPT(HELP):
      case COPT(VERSION):
        // These are special in that they are allowed directly rather than
        // following -Xtidy.  Consequently, they must be printed seperately
        // (below).
        continue;
    } // switch
    fprintf( fout, "  --%s", option->name );
    size_t long_opt_len = strlen( option->name );
    switch ( option->has_arg ) {
      case no_argument:
        break;
      case optional_argument:
        // LCOV_EXCL_START: tidy doesn't currently have any optional arguments.
        long_opt_len += STATIC_CAST( size_t, fprintf( fout, "[=ARG]" ) );
        break;
        // LCOV_EXCL_STOP
      case required_argument:
        long_opt_len += STATIC_CAST( size_t, fprintf( fout, "=ARG" ) );
        break;
    } // switch
    assert( long_opt_len <= longest_opt_len );
    FPUTNSP( longest_opt_len - long_opt_len, fout );
    fprintf( fout, " (-%c) %s.\n", option->val, get_opt_help( option->val ) );
  } // for

  fputs( "\nother options:\n", fout );

  FOREACH_CLI_OPTION( option, OPTIONS ) {
    switch ( option->val ) {
      case COPT(HELP):
      case COPT(VERSION):
        break;
      default:
        continue;
    } // switch
    fprintf( fout, "  --%s", option->name );
    size_t const long_opt_len = strlen( option->name );
    assert( option->has_arg == no_argument );
    assert( long_opt_len <= longest_opt_len );
    FPUTNSP( longest_opt_len - long_opt_len, fout );
    fprintf( fout, " (-%c) %s.\n", option->val, get_opt_help( option->val ) );
  } // for

  fputs(
    "\n"
    PACKAGE_NAME " home page: " PACKAGE_URL "\n"
    "Report bugs to: " PACKAGE_BUGREPORT "\n",
    fout
  );

  exit( status );
}

/**
 * Convenience macro for printing a `configure` option.
 *
 * @param OPT The option string literal to print (without the leading `--`).
 */
#define PUT_CONFIG_OPT(OPT) BLOCK( \
  PUTS( "\n  --" OPT ); printed_opt = true; )

// LCOV_EXCL_START
/**
 * Prints the **symbol-tidy** version.
 *
 * @param verbose If `true`, prints configure feature &amp; package options.
 */
static void print_version( bool verbose ) {
  PUTS(
    PACKAGE_STRING "\n"
    "Copyright (C) " TIDY_COPYRIGHT_YEAR " " TIDY_AUTHOR "\n"
    "License " TIDY_LICENSE " <" TIDY_LICENSE_URL ">.\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY to the extent permitted by law.\n"
  );
  if ( !verbose )
    return;

  PUTS( "\nconfigure feature & package options:" );
  bool printed_opt = false;
#ifdef NDEBUG
  PUT_CONFIG_OPT( "disable-assert" );
#endif /* NDEBUG */
#ifdef ENABLE_ASAN
  PUT_CONFIG_OPT( "enable-asan" );
#endif /* ENABLE_ASAN */
#ifdef ENABLE_COVERAGE
  PUT_CONFIG_OPT( "enable-coverage" );
#endif /* ENABLE_COVERAGE */
#ifdef ENABLE_MSAN
  PUT_CONFIG_OPT( "enable-msan" );
#endif /* ENABLE_MSAN */
#ifdef ENABLE_UBSAN
  PUT_CONFIG_OPT( "enable-ubsan" );
#endif /* ENABLE_UBSAN */
  if ( !printed_opt )
    PUTS( " none" );
  putchar( '\n' );
}
// LCOV_EXCL_STOP

/**
 * Initializes every tidy_source's \ref tidy_source::ptu_argv.
 *
 * @param sources The sources to initialize.
 * @param argc The command-line argument count from `main()`.
 * @param argv The command-line argument values from `main()`.
 * @param compiler_path C/C++ compiler path, if any.
 */
static void sources_init_argv( array_t *sources, int argc,
                               char const *const argv[],
                               char const *compiler_path ) {
  assert( sources != NULL );
  assert( argv != NULL );

  static char const *const LIBCLANG_ARGS[] = {
    "-D" TIDY_COMPILER_ID,
    "-Qunused-arguments",
    "-Wno-unknown-warning-option",
  };

  bool did_lang[] = { false, false };   // Do each ptu_argv[i] at most once.

  for ( size_t i = 0; i < sources->len; ++i ) {
    tidy_source *const source = array_at_nc( sources, i );
    bool const is_cxx = source->lang == CXLanguage_CPlusPlus;
    source->ptu_argv = &ptu_argv[ is_cxx ];
    if ( did_lang[ is_cxx ] )
      continue;

    array_t *const pargv = &ptu_argv[ is_cxx ];
    FOREACH_ARRAY_ELEMENT( char const*, parg, LIBCLANG_ARGS )
      *(char const**)array_push_back( pargv ) = *parg;

    for ( int j = 1; j < argc; ++j )
      *(char const**)array_push_back( pargv ) = argv[j];

    did_lang[ is_cxx ] = true;
  } // for

  if ( compiler_path == NULL )
    return;

  if ( did_lang[0] )
    argv_add_include_paths( &ptu_argv[0], compiler_path, CXLanguage_C );
  if ( did_lang[1] )
    argv_add_include_paths( &ptu_argv[1], compiler_path, CXLanguage_CPlusPlus );
}

////////// extern functions ///////////////////////////////////////////////////

void cli_options_init( int argc, char const *argv[], array_t *rv_sources ) {
  assert( argc > 0 );
  assert( argv != NULL );
  assert( rv_sources != NULL );
  ASSERT_RUN_ONCE();

  strbuf_t      err_buf = STRBUF_INIT();
  char const   *opt_compiler_path = OPT_COMPILER_DEFAULT;
  char const   *opt_directory = NULL;
  bool          opt_help = false;
  unsigned      opt_version = 0;
  char *const   short_opts = make_short_opts( OPTIONS );
  int           tidy_argc;
  char const  **tidy_argv;

  move_tidy_args( &argc, argv, &tidy_argc, &tidy_argv );

  opterr = 0;                           // suppress default error message
  for (;;) {
    int const short_opt = getopt_long(
      tidy_argc, CONST_CAST( char**, tidy_argv ), short_opts, OPTIONS,
      /*longindex=*/NULL
    );
    if ( short_opt == -1 )
      break;
    switch ( short_opt ) {
      case COPT(COLOR):
        if ( !opt_color_parse( optarg ) ) {
          fatal_error( EX_USAGE,
            "\"%s\": invalid value for %s\n",
            optarg, get_opt_format( short_opt )
          );
        }
        break;
      case COPT(COMPILER):
        opt_compiler_path = strcmp( optarg, "none" ) == 0 ? NULL : optarg;
        break;
      case COPT(CONFIG):
        opt_config_path = optarg;
        break;
      case COPT(DEBUG):
        opt_debug = true;
        break;
      case COPT(DIRECTORY):
        opt_directory = optarg;
        break;
      case COPT(ERROR):
        if ( !opt_error_parse( optarg ) ) {
          fatal_error( EX_USAGE,
            "\"%s\": invalid value for %s;"
            " must be one of \"violations\", \"always\", or \"never\"\n",
            optarg, get_opt_format( short_opt )
          );
        }
        break;
      case COPT(HELP):
        opt_help = true;
        break;
      case COPT(NO_CONFIG_LAYERS):
        opt_config_layers = false;
        break;
      case COPT(VERBOSE):
        if ( !opt_verbose_parse( optarg ) ) {
          fatal_error( EX_USAGE,
            "\"%s\": invalid value for %s; must be [" OPT_VERBOSE_ALL "]+\n",
            optarg, get_opt_format( short_opt )
          );
        }
        break;
      case COPT(VERSION):
        ++opt_version;
        break;

      case ':':
        // LCOV_EXCL_START: move_tidy_args() checks for missing arguments out
        // of necessity, so this case is never executed; but keep the code just
        // in case.
        fatal_error( EX_USAGE,
          "\"%s\" requires an argument\n",
          get_opt_format( short_opt == ':' ? optopt : short_opt )
        );
        // LCOV_EXCL_STOP

      case '?':;
        // Determine whether the invalid option was short or long.
        char const *const invalid_opt = tidy_argv[ optind - 1 ];
        if ( invalid_opt != NULL && STRNCMPLIT( invalid_opt, "--" ) == 0 )
          strbuf_printf( &err_buf, "\"%s\"", invalid_opt + STRLITLEN( "--" ) );
        else
          strbuf_printf( &err_buf, "'%c'", optopt );
        fatal_error( EX_USAGE, "%s: %s\n", err_buf.str, INVALID_XTIDY_OPTION );

      default:
        // LCOV_EXCL_START
        if ( isprint( short_opt ) )
          strbuf_printf( &err_buf, "'%c'", short_opt );
        else
          strbuf_printf( &err_buf, "%d", short_opt );
        INTERNAL_ERROR(
          "%s: unhandled getopt_long() return value\n", err_buf.str
        );
        // LCOV_EXCL_STOP
    } // switch

    option_mark_set( short_opt );
  } // for

  free( short_opts );
  check_options();

  int const new_argc = get_sources( argc, argv, rv_sources );
  sources_init_argv( rv_sources, new_argc, argv, opt_compiler_path );

  if ( IS_VERBOSE( ARGS ) ) {
    // LCOV_EXCL_START: -I, -isystem, etc., options contain absolute paths.
    array_t const *const libclang_argv =
      ptu_argv[1].len > 0 ? &ptu_argv[1] : &ptu_argv[0];
    verbose_print_argv( "libclang",
      STATIC_CAST( int, libclang_argv->len ),
      libclang_argv->elements
    );
    verbose_print_argv( "tidy", tidy_argc, tidy_argv );
    // LCOV_EXCL_STOP
  }

  free( tidy_argv );

  if ( opt_help )
    print_usage( argc > 1 ? EX_USAGE : EX_OK );
  if ( opt_version > 0 ) {
    if ( argc > 1 )                     // symbol-tidy --version foo
      print_usage( EX_USAGE );
    // LCOV_EXCL_START
    print_version( /*verbose=*/opt_version > 1 );
    exit( EX_OK );
    // LCOV_EXCL_STOP
  }
  if ( rv_sources->len == 0 )
    fatal_error( EX_USAGE, "no source files given\n" );

  if ( opt_directory != NULL ) {
    if ( IS_VERBOSE( DIRECTORY ) ) {
      verbose_section_begin( /*printed_header=*/NULL );
      verbose_printf( "directory = \"%s\"\n", opt_directory );
    }
    if ( chdir( opt_directory ) != 0 )
      fatal_error( EX_IOERR, "\"%s\": %s\n", opt_directory, STRERROR() );
  }
}

bool option_is_set( int short_opt ) {
  assert( short_opt > 0 );
  assert( STATIC_CAST( size_t, short_opt ) < ARRAY_SIZE( is_opt_set ) );
  return is_opt_set[ short_opt ];
}

void option_mark_set( int short_opt ) {
  assert( short_opt > 0 );
  assert( STATIC_CAST( size_t, short_opt ) < ARRAY_SIZE( is_opt_set ) );
  assert( isalnum( short_opt ) );
  is_opt_set[ short_opt ] = true;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
