/*
**      symbol-tidy -- Symbol tidier
**      src/config_file.c
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
 * Defines functions for reading and querying an **symbol-tidy**
 * configuration file.
 */

// local
#include "pjl_config.h"
#include "config_file.h"
#include "bit_util.h"
#include "cli_options.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "options.h"
#include "path_util.h"
#include "print.h"
#include "str_util.h"
#include "strbuf.h"
#include "symbol-tidy.h"
#include "toml_lite.h"
#include "type_traits.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <errno.h>
#if HAVE_PWD_H
# include <pwd.h>                       /* for getpwuid() */
#endif /* HAVE_PWD_H */
#include <stdbool.h>                    /* for uint64_t */
#include <stdio.h>
#include <stdlib.h>                     /* for getenv(), ... */
#include <string.h>
#include <sysexits.h>
#include <unistd.h>                     /* for geteuid() */

/// @endcond

/**
 * @addtogroup config-file-group
 * @{
 */

////////// macros /////////////////////////////////////////////////////////////

/**
 * The name of the **symbol-tidy** table.
 */
#define TABLE_SYMBOL_TIDY_NAME    "symbol-tidy"

////////// enums //////////////////////////////////////////////////////////////

/**
 * Options for the config_open() function.
 */
enum config_opts {
  CONFIG_OPT_ERROR_IS_FATAL = 1 << 0,   ///< An error is fatal.
  CONFIG_OPT_IGNORE_ENOENT  = 1 << 1    ///< Ignore file not found.
};

/**
 * Table kind.
 */
enum config_table_kind {
  TABLE_NONE        = 0,                ///< No table.
  TABLE_SYMBOL_TIDY = 1 << 0,           ///< The `symbol-tidy` table.
  TABLE_SOURCE      = 1 << 1,           ///< Source table.
  TABLE_SYMBOL      = 1 << 2            ///< Symbol table.
};

/// @cond DOXYGEN_IGNORE
#define TABLE_TIDY_OR_SOURCE      (TABLE_SYMBOL_TIDY | TABLE_SOURCE)
/// @endcond

////////// typedefs ///////////////////////////////////////////////////////////

typedef enum    config_opts           config_opts;
typedef struct  config_key            config_key;
typedef struct  config_parse_fn_args  config_parse_fn_args;
typedef enum    config_table_kind     config_table_kind;
typedef struct  ignore_symbol         ignore_symbol;

/**
 * Signature for a function that parses the value of a configuration key.
 *
 * @note Functions ending in `_string` are functions that assume the \ref
 * toml_value::type "type" of \a value is #TOML_STRING.
 *
 * @param config The configuration arguments to use.
 */
typedef void (*config_parse_fn)( config_parse_fn_args const *config );

////////// structs ////////////////////////////////////////////////////////////

/**
 * Configuration key (in a table).
 */
struct config_key {
  char const       *name;               ///< Key name.
  config_table_kind ok_table_kinds;     ///< Table kind(s) OK in.
  config_parse_fn   parse_fn;           ///< Value parsing function.
};

/**
 * Arguments to a config_parse_fn.
 */
struct config_parse_fn_args {
  char const       *config_path;        ///< Configuration file path.
  toml_table const *table;              ///< Current table.
  config_table_kind table_kind;         ///< Current table kind.
  toml_key const   *key;                ///< Current key.
  toml_value const *value;              ///< Value to parse.
};

/**
 * A symbol to ignore set via the `ignore-symbols` key.
 */
struct ignore_symbol {
  char const *path;                     ///< Symbol path; if NULL, any path.
  char const *name;                     ///< Symbol name.
};

////////// local functions ////////////////////////////////////////////////////

static void         color_parse( config_parse_fn_args const* );

NODISCARD
static FILE*        config_open( char const*, config_opts );

static void         error_parse( config_parse_fn_args const* );

NODISCARD
static char const*  home_dir( void );

static void         ignore_paths_parse( config_parse_fn_args const* );
static void         ignore_paths_parse_string( config_parse_fn_args const* );
static void         ignore_symbol_cleanup( ignore_symbol* );
static void         ignore_symbols_parse( config_parse_fn_args const* );
static void         ignore_symbols_parse_string( config_parse_fn_args const* );
static void         print_statistics( void );
static void         print_invalid_value( config_parse_fn_args const*,
                                         toml_value const*, toml_type );

static void         toml_value_print( toml_value const*, FILE* );

////////// local constants ////////////////////////////////////////////////////

/**
 * Configuration keys.
 */
static config_key const CONFIG_KEYS[] = {
  { "color",                  TABLE_SYMBOL_TIDY,    &color_parse          },
  { "error",                  TABLE_SYMBOL_TIDY,    &error_parse          },
  { "ignore-paths",           TABLE_SYMBOL_TIDY,    &ignore_paths_parse   },
  { "ignore-symbols",         TABLE_TIDY_OR_SOURCE, &ignore_symbols_parse },
};

/**
 * Strings for table kinds.
 */
static char const *const TABLE_KINDS[] = {
  [ TABLE_NONE ] = "none",
  [ TABLE_SYMBOL_TIDY ] = TABLE_SYMBOL_TIDY_NAME,
  [ TABLE_TIDY_OR_SOURCE ] = TABLE_SYMBOL_TIDY_NAME " or source",
  [ TABLE_SOURCE ] = "source",
  [ TABLE_SYMBOL ] = "symbol"
};

////////// local variables ////////////////////////////////////////////////////

static unsigned     error_count;          ///< Configuration file error count.
static hash_table_t ignore_path_set;      ///< Set of paths to ignore.
static hash_table_t ignore_symbol_set;    ///< Set of symbols to ignore.
static unsigned     warning_count;        ///< Configuration file warning count.

////////// local TOML type parsing functions //////////////////////////////////

/**
 * Parses either a TOML string value or an array of string values.
 *
 * @param config The config_parse_fn_args to use.
 * @param parse_fn The parse function to use.
 *
 * @sa toml_string_parse()
 */
static
void toml_string_or_string_array_parse( config_parse_fn_args const *config,
                                        config_parse_fn parse_fn ) {
  assert( config != NULL );
  assert( parse_fn != NULL );

  switch ( config->value->type ) {
    case TOML_STRING:
      (*parse_fn)( config );
      break;

    case TOML_ARRAY:
      for ( unsigned i = 0; i < config->value->a.size; ++i ) {
        toml_value const *const a_value = &config->value->a.values[i];
        if ( a_value->type != TOML_STRING ) {
          print_invalid_value( config, a_value, TOML_STRING );
          continue;
        }
        config_parse_fn_args const a_value_config = {
          .config_path = config->config_path,
          .table = config->table,
          .table_kind = config->table_kind,
          .key = config->key,
          .value = a_value
        };
        (*parse_fn)( &a_value_config );
      } // for
      break;

    default:
      print_file_error(
        config->config_path, config->value->loc.line, config->value->loc.col,
        ""
      );
      toml_value_print( config->value, stderr );
      EPRINTF(
        ": invalid value for \"%s\"; expected string or array\n",
        config->key->name
      );
      ++error_count;
  } // switch
}

/**
 * Parses a TOML string value.
 *
 * @param config The config_parse_fn_args to use.
 * @param rv_string Receives a pointer to the string.
 * @return Returns `true` only if a string was parsed successfully.
 *
 * @sa toml_string_or_string_array_parse()
 */
NODISCARD
static bool toml_string_parse( config_parse_fn_args const *config,
                               char const **rv_string ) {
  assert( config != NULL );
  assert( rv_string != NULL );

  if ( config->value->type != TOML_STRING ) {
    print_invalid_value( config, /*value=*/NULL, TOML_STRING );
    return false;
  }

  *rv_string = config->value->s;
  return true;
}

////////// local configutation key parsing functions //////////////////////////

/**
 * Parses the value of an `"color"` key.
 *
 * @param config The config_parse_fn_args to use.
 */
static void color_parse( config_parse_fn_args const *config ) {
  char const *string_value;
  if ( !toml_string_parse( config, &string_value ) )
    return;
  if ( option_is_set( COPT(COLOR) ) )
    return;                             // LCOV_EXCL_LINE
  if ( !opt_color_parse( string_value ) ) {
    print_invalid_value( config, /*value=*/NULL, TOML_STRING );
    EPUTS(
      "; must be one of "
      "\"always\", \"auto\", \"isatty\", \"never\", "
      "\"not_file\", \"not_isreg\", or \"tty\"\n"
    );
    return;
  }
  option_mark_set( COPT(COLOR) );
}

/**
 * Parses the value of a `"error"` key.
 *
 * @param config The config_parse_fn_args to use.
 */
static void error_parse( config_parse_fn_args const *config ) {
  char const *string_value;
  if ( !toml_string_parse( config, &string_value ) )
    return;
  if ( option_is_set( COPT(ERROR) ) )
    return;                             // LCOV_EXCL_LINE
  if ( !opt_error_parse( string_value ) ) {
    print_invalid_value( config, /*value=*/NULL, TOML_STRING );
    EPUTS( "; must be one of \"always\", \"never\", or \"violations\"\n" );
  }
  option_mark_set( COPT(ERROR) );
}

/**
 * Parses the value of an `"ignore-files"` key.
 *
 * @param config The config_parse_fn_args to use.
 */
static void ignore_paths_parse( config_parse_fn_args const *config ) {
  toml_string_or_string_array_parse( config, &ignore_paths_parse_string );
};

/**
 * Parses a single string value of an `"ignore-symbols"` key.
 *
 * @param config The config_parse_fn_args to use.
 *
 * @sa ignore_symbols_parse()
 */
static void ignore_paths_parse_string( config_parse_fn_args const *config ) {
  assert( config != NULL );
  assert( config->value->type == TOML_STRING );

  ht_insert_rv_t const hti = ht_table_insert(
    &ignore_path_set, CONST_CAST( char*, config->value->s ),
    strlen( config->value->s ) + 1/*\0*/
  );
  if ( !hti.inserted ) {
    print_file_warning(
      config->config_path, config->value->loc.line, config->value->loc.col,
      "\"%s\" already ignored\n",
      config->value->s
    );
    ++warning_count;
  }
}

/**
 * Parses the value of an `"ignore-symbols"` key.
 *
 * @param config The config_parse_fn_args to use.
 *
 * @sa ignore_symbols_parse_string()
 */
static void ignore_symbols_parse( config_parse_fn_args const *config ) {
  toml_string_or_string_array_parse( config, &ignore_symbols_parse_string );
}

/**
 * Parses a single string value of an `"ignore-symbols"` key.
 *
 * @param config The config_parse_fn_args to use.
 *
 * @sa ignore_symbols_parse()
 */
static void ignore_symbols_parse_string( config_parse_fn_args const *config ) {
  assert( config != NULL );
  assert( config->value->type == TOML_STRING );

  ignore_symbol new_igsym = { .name = config->value->s };
  ht_insert_rv_t const hti =
    ht_table_insert( &ignore_symbol_set, &new_igsym, sizeof new_igsym );
  if ( hti.inserted ) {
    ignore_symbol *const igsym = HT_DINT( hti.entry );
    igsym->name = strdup_or_exit( igsym->name );
    if ( config->table_kind != TABLE_SYMBOL_TIDY )
      igsym->path = strdup_or_exit( config->table->key.name );
  }
  else {
    print_file_warning(
      config->config_path, config->value->loc.line, config->value->loc.col,
      "\"%s\" already ignored\n",
      config->value->s
    );
    ++warning_count;
  }
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Cleans-up all configuration data.
 */
static void config_cleanup( void ) {
  print_statistics();
  ht_table_cleanup( &ignore_path_set, /*free_fn=*/NULL );
  ht_table_cleanup(
    &ignore_symbol_set, POINTER_CAST( ht_free_fn_t, &ignore_symbol_cleanup )
  );
}

/**
 * Finds and opens the configuration file.
 *
 * @remarks
 * @parblock
 * Configuration files are opened (if they exist) in this order:
 *
 *  1. The path given as the value for either the `--config` or `-c` command-
 *     line option.  (If specified, this path _must_ exist.)
 *  2. `$PWD/symbol-tidy.toml`.
 *  3. `$XDG_CONFIG_HOME/symbol-tidy/config.toml`.  If `XDG_CONFIG_HOME` is
 *     unset or empty, it defaults to `~/.config`.
 *  4. For each _path_ in a colon separated list of paths in `XDG_CONFIG_DIRS`,
 *     the first of <i>path</i><tt>/symbol-tidy/config.toml</tt> to exist and
 *     be readable.  If `XDG_CONFIG_DIRS` is unset or empty, it defaults to
 *     `/etc/xdg`.
 * @endparblock
 *
 * @param config_path The configuration file path.  May be NULL.
 * @param rv_path_buf A path to receive the path of the configuration file that
 * was found, if any.
 * @return Returns the `FILE*` for the configuration file if found or NULL if
 * not.
 */
NODISCARD
static FILE* config_file_find( char const *config_path,
                               strbuf_t *rv_path_buf ) {
  assert( rv_path_buf != NULL );

  // This must be incremented in every case below and not just once initally
  // due to the fallthroughs between cases.
  static unsigned case_num = 1;

  FILE *config_file = NULL;
  strbuf_t path_buf = STRBUF_INIT();

  switch ( case_num ) {
    case 1:
      // Try --config/-c command-line option.
      ++case_num;
      config_file = config_open( config_path, CONFIG_OPT_ERROR_IS_FATAL );
      if ( config_file != NULL ) {
        strbuf_puts( &path_buf, config_path );
        break;
      }
      FALLTHROUGH;

    case 2:
      // Try $PWD/symbol-tidy.toml.
      ++case_num;
      size_t cwd_path_len;
      char const *const cwd_path = path_cwd( &cwd_path_len );
      strbuf_reset( &path_buf );
      strbuf_putsn( &path_buf, cwd_path, cwd_path_len );
      strbuf_paths( &path_buf, PACKAGE ".toml" );
      config_file = config_open( path_buf.str, CONFIG_OPT_IGNORE_ENOENT );
      if ( config_file != NULL )
        break;
      FALLTHROUGH;

    case 3:
      // Try $XDG_CONFIG_HOME/symbol-tidy/config.toml or
      // $HOME/.config/symbol-tidy/config.toml.
      ++case_num;
      strbuf_reset( &path_buf );
      char const *const config_home =
        null_if_empty( getenv( "XDG_CONFIG_HOME" ) );
      if ( config_home != NULL ) {
        strbuf_puts( &path_buf, config_home );
      }
      else {
        char const *const home = home_dir();
        if ( home != NULL ) {
          strbuf_puts( &path_buf, home );
          strbuf_paths( &path_buf, ".config" );
        }
      }
      if ( path_buf.len > 0 ) {
        strbuf_paths( &path_buf, PACKAGE );
        strbuf_paths( &path_buf, "config.toml" );
        config_file = config_open( path_buf.str, CONFIG_OPT_IGNORE_ENOENT );
        if ( config_file != NULL )
          break;
      }
      FALLTHROUGH;

    case 4:
      // Try $XDG_CONFIG_DIRS/symbol-tidy/config.toml or
      // /etc/xdg/symbol-tidy/config.toml.
      ++case_num;
      char const *config_dirs = null_if_empty( getenv( "XDG_CONFIG_DIRS" ) );
      if ( config_dirs == NULL ) {
        if ( (opt_test & TIDY_TEST_NO_ETC_XDG) == 1 )
          break;
        config_dirs = "/etc/xdg";       // LCOV_EXCL_LINE
      }
      for (;;) {
        char const *const next_sep = strchr( config_dirs, ':' );
        size_t const dir_len = next_sep != NULL ?
          STATIC_CAST( size_t, next_sep - config_dirs ) :
          strlen( config_dirs );
        if ( dir_len > 0 ) {
          strbuf_reset( &path_buf );
          strbuf_putsn( &path_buf, config_dirs, dir_len );
          strbuf_paths( &path_buf, PACKAGE );
          strbuf_paths( &path_buf, "config.toml" );
          config_file = config_open( path_buf.str, CONFIG_OPT_IGNORE_ENOENT );
          if ( config_file != NULL )
            break;
        }
        if ( next_sep == NULL )
          break;
        config_dirs = next_sep + 1;
      } // for
  } // switch

  if ( config_file != NULL )
    *rv_path_buf = path_buf;
  else
    strbuf_cleanup( &path_buf );
  return config_file;
}

/**
 * Attempts to find the config_key having \a key_name.
 *
 * @param key_name The name of the configuration key to find.
 * @return Returns a pointer to the corresponding config_key or NULL if \a
 * key_name does not correspond to a config_key.
 */
NODISCARD
static config_key const* config_key_find( char const *key_name ) {
  assert( key_name != NULL );

  // For such a small set, linear search is good enough.
  FOREACH_ARRAY_ELEMENT( config_key, key, CONFIG_KEYS ) {
    if ( strcmp( key_name, key->name ) == 0 )
      return key;
  } // for

  return NULL;
}

/**
 * Tries to open a configuration file given by \a config_path.
 *
 * @param config_path The configuration file path.  May be NULL.
 * @param opts The configuration options, if any.
 * @return Returns a `FILE*` to the open file upon success or NULL upon either
 * error or if \a config_path is NULL.
 */
NODISCARD
static FILE* config_open( char const *config_path, config_opts opts ) {
  if ( config_path == NULL )
    return NULL;

  FILE *const config_file = fopen( config_path, "r" );
  bool const  ok = config_file != NULL;
  static bool printed_configuration_header;

  if ( IS_VERBOSE( CONFIG_FILES ) ) {
    // LCOV_EXCL_START: configuration paths are absolute
    if ( verbose_section_begin( &printed_configuration_header ) )
      verbose_printf( "configuration files:\n" );
    verbose_printf( "  \"%s\": %s\n", config_path, ok ? "OK" : STRERROR() );
    // LCOV_EXCL_STOP
  }

  if ( !ok ) {
    switch ( errno ) {
      case ENOENT:
        if ( (opts & CONFIG_OPT_IGNORE_ENOENT) != 0 )
          break;
        FALLTHROUGH;
      default:
        if ( (opts & CONFIG_OPT_ERROR_IS_FATAL) != 0 ) {
          print_file_error( config_path, 0, 0, "%s\n", STRERROR() );
          exit( EX_NOINPUT );
        }
        print_file_warning( config_path, 0, 0, "%s\n", STRERROR() );
        ++warning_count;
        break;
    } // switch
  }

  return config_file;
}

/**
 * Parses a configuration file.
 *
 * @param config_path The configurarion file path.
 * @param config_file The `FILE*` corresponding to \a config_path.
 */
static void config_parse( char const *config_path, FILE *config_file ) {
  assert( config_path != NULL );
  assert( config_file != NULL );

  toml_file toml;
  toml_file_init( &toml, config_file );

  toml_table table;
  toml_table_init( &table );

  while ( toml_table_next( &toml, &table ) ) {
    if ( table.key.name == NULL ) {
      print_file_error(
        config_path, toml.loc.line, 1,
        "required table header missing\n"
      );
      ++error_count;
      continue;
    }
    if ( toml_table_empty( &table ) ) {
      print_file_warning(
        config_path, table.key.loc.line, table.key.loc.col,
        "\"%s\": empty table\n", table.key.name
      );
      ++warning_count;
      continue;
    }

    config_table_kind table_kind =
      strcmp( table.key.name, TABLE_SYMBOL_TIDY_NAME ) == 0 ?
        TABLE_SYMBOL_TIDY : TABLE_NONE;
    config_table_kind ok_table_kinds = table_kind;

    toml_iterator iter;
    toml_iterator_init( &iter, &table );
    for ( toml_key_value const *kv;
          (kv = toml_iterator_next( &iter )) != NULL; ) {
      config_key const *const found_key = config_key_find( kv->key.name );
      if ( found_key == NULL ) {
        print_file_error(
          config_path, kv->key.loc.line, kv->key.loc.col,
          "\"%s\": unknown key\n", kv->key.name
        );
        ++error_count;
        continue;
      }

      if ( table_kind == TABLE_NONE ) {
        table_kind = found_key->ok_table_kinds &
                     ~TO_UNSIGNED_EXPR( TABLE_SYMBOL_TIDY );
        assert( is_1_bit( table_kind ) );
      }

      if ( ok_table_kinds == TABLE_NONE ||
           is_0n_bit_only_in_set( found_key->ok_table_kinds,
                                  ok_table_kinds ) ) {
        ok_table_kinds = found_key->ok_table_kinds;
      }
      else if ( (found_key->ok_table_kinds & ok_table_kinds) == 0 ) {
        assert( ok_table_kinds < ARRAY_SIZE( TABLE_KINDS ) );
        print_file_error(
          config_path, kv->key.loc.line, kv->key.loc.col,
          "\"%s\": key not allowed in %s table%s; allowed only in %s table%s\n",
          found_key->name,
          TABLE_KINDS[ ok_table_kinds ],
          is_1_bit( ok_table_kinds ) ? "" : "s",
          TABLE_KINDS[ found_key->ok_table_kinds ],
          is_1_bit( found_key->ok_table_kinds ) ? "" : "s"
        );
        ++error_count;
        continue;
      }

      config_parse_fn_args const config = {
        .config_path = config_path,
        .table = &table,
        .table_kind = table_kind,
        .key = &kv->key,
        .value = &kv->value
      };
      (*found_key->parse_fn)( &config );
    } // for
  } // while

  toml_table_cleanup( &table );

  if ( toml.error ) {
    print_file_error(
      config_path, toml.loc.line, toml.loc.col,
      "%s\n", toml_error_msg( &toml )
    );
    ++error_count;
  }

  toml_file_cleanup( &toml );

  if ( warning_count > 0 || error_count > 0 ) {
    EPRINTF( "%s: ", prog_name );
    if ( warning_count > 0 )
      EPRINTF( "%u warning%s", warning_count, plural_s( warning_count ) );
    if ( error_count > 0 ) {
      if ( warning_count > 0 )
        EPUTS( " and " );
      EPRINTF( "%u error%s", error_count, plural_s( error_count ) );
    }
    EPUTS( " generated\n" );
    if ( error_count > 0 )
      exit( EX_CONFIG );
  }
}

/**
 * Gets the full path of the user's home directory.
 *
 * @return Returns said directory or NULL if it is not obtainable.
 */
NODISCARD
static char const* home_dir( void ) {
  static char const *home;

  RUN_ONCE {
    if ( (opt_test & TIDY_TEST_NO_HOME) == 0 ) {
      home = null_if_empty( getenv( "HOME" ) );
#if HAVE_GETEUID && HAVE_GETPWUID && HAVE_STRUCT_PASSWD_PW_DIR
      if ( home == NULL ) {
        // LCOV_EXCL_START
        struct passwd const *const pw = getpwuid( geteuid() );
        if ( pw != NULL )
          home = null_if_empty( pw->pw_dir );
        // LCOV_EXCL_STOP
      }
#endif /* HAVE_GETEUID && && HAVE_GETPWUID && HAVE_STRUCT_PASSWD_PW_DIR */
    }
  }

  return home;
}

/**
 * Cleans-up an ignore_symbol.
 *
 * @param igsym The ignore_symbol to clean-up.
 */
static void ignore_symbol_cleanup( ignore_symbol *igsym ) {
  if ( igsym != NULL ) {
    FREE( igsym->name );
    FREE( igsym->path );
  }
}

/**
 * Compares to \ref ignore_symbol objects.
 *
 * @param i_igsym The first ignore_symbol.
 * @param j_igsym The second ignore_symbol.
 * @return eturns a number less than 0, 0, or greater than 0 if the name of \a
 * i_igsym is less than, equal to, or greater than the name of \a j_igsym,
 * respectively.
 */
static int ignore_symbol_cmp( ignore_symbol const *i_igsym,
                              ignore_symbol const *j_igsym ) {
  int const cmp = strcmp( i_igsym->name, j_igsym->name );
  if ( cmp != 0 )
    return cmp;
  if ( i_igsym->path == NULL || j_igsym->path == NULL )
    return 0;
  return strcmp( i_igsym->path, j_igsym->path );
}

/**
 * Calculates the hash of \a igsym.
 *
 * @param igsym The ignore_symbol to hash.
 * @return Returns said hash.
 */
static fnv1a_t ignore_symbol_hash( ignore_symbol const *igsym ) {
  return fnv1a_s( igsym->name );
}

/**
 * Prints an error message saying that \a value is invalid.
 *
 * @param config The config_parse_fn_args to use.
 * @param value The toml_value to use.  If NULL, then \a config's \ref
 * config_parse_fn_args::value "value" is used instead.
 * @param expected The expected type.  If not equal to \a value's type, then
 * <tt>&quot;; expected </tt><i>type</i><tt>&quot;</tt> is printed followed by
 * a newline; otherwise no newline is printed.
 *
 * @note \ref error_count is incremented.
 */
static void print_invalid_value( config_parse_fn_args const *config,
                                 toml_value const *value, toml_type expected ) {
  assert( config != NULL );
  if ( value == NULL )
    value = config->value;

  print_file_error( config->config_path, value->loc.line, value->loc.col, "" );
  toml_value_print( value, stderr );
  EPRINTF( ": invalid value for \"%s\"", config->key->name );
  if ( value->type != expected )
    EPRINTF( "; expected %s\n", toml_type_name( expected ) );

  ++error_count;
}

/**
 * Prints statistics if requested.
 */
static void print_statistics( void ) {
  if ( !verbose_print_statistics() )
    return;

  verbose_printf(
    "  ignore-path set:\n"
  );
  verbose_printf(
    "    ips-load-factor = " TIDY_STAT_LF_FMT "\n",
                           ht_table_load_factor( &ignore_path_set )
  );
  verbose_printf(
    "    ips-size = %u\n", ignore_path_set.size
  );

  verbose_printf(
    "  ignored-symbol set:\n"
  );
  verbose_printf(
    "    iss-load-factor = " TIDY_STAT_LF_FMT "\n",
                           ht_table_load_factor( &ignore_symbol_set )
  );
  verbose_printf(
    "    iss-size = %u\n", ignore_symbol_set.size
  );
}

/**
 * Prints a TOML value.
 *
 * @param value The toml_value to print.
 * @param fout The `FILE` to print to.
 */
static void toml_value_print( toml_value const *value, FILE *fout ) {
  assert( value != NULL );

  switch ( value->type ) {
    case TOML_BOOL:
      fprintf( fout, "\"%s\"", value->b ? "true" : "false" );
      break;
    case TOML_INT:
      fprintf( fout, "\"%ld\"", value->i );
      break;
    case TOML_STRING:
      fprintf( fout, "\"%s\"", value->s );
      break;
    case TOML_ARRAY:
      fputs( "array", fout );
      break;
  } // switch
}

////////// extern functions ///////////////////////////////////////////////////

void config_init( void ) {
  ASSERT_RUN_ONCE();

  ht_table_init(
    &ignore_path_set, HT_DINT, 2.0, 10,
    POINTER_CAST( ht_cmp_fn_t, &strcmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );
  ht_table_init(
    &ignore_symbol_set, HT_DINT, 2.0, 10,
    POINTER_CAST( ht_cmp_fn_t, &ignore_symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &ignore_symbol_hash )
  );
  ATEXIT( &config_cleanup );

  bool found_at_least_1 = false;
  strbuf_t path_buf = STRBUF_INIT();
  do {
    FILE *const config_file = config_file_find( opt_config_path, &path_buf );
    if ( config_file == NULL )
      break;
    config_parse( path_buf.str, config_file );
    fclose( config_file );
    found_at_least_1 = true;
    strbuf_reset( &path_buf );
    warning_count = 0;
  } while ( opt_config_layers || !found_at_least_1 );
  strbuf_cleanup( &path_buf );
}

bool config_path_is_ignored( char const *path ) {
  return ht_table_find( &ignore_path_set, path ) != NULL;
}

bool config_symbol_is_ignored( char const *sym_name, char const *sym_path ) {
  ignore_symbol const find_igsym = { .name = sym_name, .path = sym_path };
  ht_entry_t const *const found_igsym =
    ht_table_find( &ignore_symbol_set, &find_igsym );
  return found_igsym != NULL;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
