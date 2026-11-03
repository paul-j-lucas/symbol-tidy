/*
**      symbol-tidy -- Symbol tidier
**      src/source.c
**
**      Copyright (C) 2026  Paul J. Lucas, et al.
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
 * Defines functions for a source file being tidied.
 */

// local
#include "pjl_config.h"
#include "source.h"
#include "clang_util.h"
#include "config_file.h"
#include "fnv1a.h"
#include "hash_table.h"
#include "path_util.h"
#include "print.h"
#include "symbol.h"
#include "symbol-tidy.h"
#include "util.h"

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>                     /* for exit(3) */
#include <string.h>
#include <sysexits.h>
#include <unistd.h>                     /* for access(2) */

/**
 * @addtogroup tidy-source-group
 * @{
 */

////////// local variables ////////////////////////////////////////////////////

static hash_table_t source_file_set;    ///< Source files being tidied.
static CXIndex      tidy_index;         ///< Libclang index.

////////// local functions ////////////////////////////////////////////////////

static void source_cpp_visit( tidy_source const*, CXFile );

////////// local functions ////////////////////////////////////////////////////

/**
 * Visits each file included to:
 *
 *  1. Add it to the global set of source files.
 *  2. Visit all preprocessor directives in it.
 *
 * @note Absolute paths, non-local files (`../`), and files explicitly ignored
 * via a config. file are skipped.
 *
 * @param included_file The file being included.
 * @param inclusion_stack The source locations of includes that lead up to \a
 * included_file being included.
 * @param inclusion_len The length of \a inclusion_stack.
 * @param data The tidy_source file whose included files are being visited.
 *
 * @sa source_cpp_visit()
 */
static void include_visitor( CXFile included_file,
                             CXSourceLocation *inclusion_stack,
                             unsigned inclusion_len, CXClientData data ) {
  (void)inclusion_stack;
  (void)inclusion_len;

  if ( included_file == NULL )
    return;

  CXString const  path_cxs = clang_getFileName( included_file );
  char const     *path_cs = clang_getCString( path_cxs );

  if ( path_cs == NULL || path_cs[0] == '\0' )
    goto skip;
  if ( path_is_absolute( path_cs ) )
    goto skip;
  path_cs = path_no_dot_slash( path_cs );
  if ( STRNCMPLIT( path_cs, "../" ) == 0 )
    goto skip;
  if ( config_path_is_ignored( path_cs ) )
    goto skip;
  if ( !source_path_add( path_cs ) )
    goto skip;

  assert( data != NULL );
  tidy_source const *const source = data;
  source_cpp_visit( source, included_file );

skip:
  clang_disposeString( path_cxs );
}

/**
 * Checks the source's translation unit for diagnostics and prints them, if
 * any.
 *
 * @note If there are errors, this function does not return.
 *
 * @param source The source file being tidied.
 */
static void source_check_diagnostics( tidy_source const *source ) {
  assert( source != NULL );
  assert( source->tu != NULL );

  unsigned const diag_count = clang_getNumDiagnostics( source->tu );
  if ( diag_count == 0 )
    return;

  unsigned error_count = 0;

  for ( unsigned i = 0; i < diag_count; ++i ) {
    CXDiagnostic const diag = clang_getDiagnostic( source->tu, i );
    enum CXDiagnosticSeverity const sev = clang_getDiagnosticSeverity( diag );
    switch ( sev ) {
      case CXDiagnostic_Error:
      case CXDiagnostic_Fatal:
        ++error_count;
        CXSourceLocation const diag_loc = clang_getDiagnosticLocation( diag );
        CXFile diag_file;
        unsigned diag_line, diag_col, diag_offset;
        clang_getSpellingLocation(
          diag_loc, &diag_file, &diag_line, &diag_col, &diag_offset
        );
        CXString const    diag_msg_cxs = clang_getDiagnosticSpelling( diag );
        char const *const diag_msg_cs = clang_getCString( diag_msg_cxs );
        CXString const    diag_path_cxs = clang_getFileName( diag_file );
        char const *const diag_path_cs = clang_getCString( diag_path_cxs );

        if ( diag_path_cs != NULL && diag_path_cs[0] != '\0' ) {
          print_file_error(
            diag_path_cs, diag_line, diag_col, "%s\n", diag_msg_cs
          );
          print_source_line( diag_path_cs, diag_line, diag_col, diag_offset );
        }
        else {
          print_error_from( "libclang", "%s\n", diag_msg_cs );
        }

        clang_disposeString( diag_msg_cxs );
        clang_disposeString( diag_path_cxs );
        break;
      default:
        /* suppress warning */;
    } // switch
    clang_disposeDiagnostic( diag );
  } // for

  if ( error_count > 0 ) {
    EPRINTF(
      "%s: %u error%s generated\n",
      prog_name, error_count, plural_s( error_count )
    );
    exit( EX_DATAERR );
  }
}

/**
 * Visits all preprocessor directives in \a file that can reference symbols to
 * mark them as roots.
 *
 * @remarks Libclang's AST doesn't include conditional preprocessor directives,
 * e.g., `#if`, `#ifdef`, etc.), so `clang_visitChildren()` can't visit them to
 * look for symbols that are referenced to mark them as used.  Consequently,
 * we're forced to iterate through all of \a source's tokens looking for `#`
 * followed by a directive name and check subsequent tokens for identifiers.
 *
 * @param source The source file for the translation unit.
 * @param file The file whose preprocessor directives to visit.
 */
static void source_cpp_visit( tidy_source const *source, CXFile file ) {
  assert( source != NULL );

  size_t file_size;
  (void)clang_getFileContents( source->tu, file, &file_size );

  CXSourceLocation const start_loc =
    clang_getLocation( source->tu, file, 1, 1 );
  CXSourceLocation const end_loc =
    clang_getLocationForOffset( source->tu, file, (unsigned)file_size );
  CXSourceRange const file_range = clang_getRange( start_loc, end_loc );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( source->tu, file_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor const tu_csr = clang_getTranslationUnitCursor( source->tu );

  for ( unsigned i = 0; i < token_count; ++i ) {
    CXSourceLocation const token_loc =
      clang_getTokenLocation( source->tu, tokens[i] );
    if ( clang_Location_isInSystemHeader( token_loc ) )
      continue;

    CXTokenKind token_kind = clang_getTokenKind( tokens[i] );
    if ( token_kind != CXToken_Punctuation )
      continue;
    if ( !tidy_Token_isSpellingEqualTo( source->tu, tokens[i], "#" ) )
      continue;
    if ( ++i == token_count )
      break;

    token_kind = clang_getTokenKind( tokens[i] );
    if ( token_kind != CXToken_Identifier )
      continue;
    int const directive_idx = tidy_Token_isSpellingEqualToAny(
      source->tu, tokens[i],
      "if", "ifdef", "ifndef", "elif", "elifdef", "elifndef"
    );
    if ( directive_idx == -1 )
      continue;

    unsigned last_line = tidy_getSpellingLocation_Line( token_loc );

    while ( ++i < token_count ) {
      unsigned const token_line =
        tidy_getTokenLocation_Line( source->tu, tokens[i] );
      if ( token_line > last_line ) {
        if ( !tidy_isEscapedNewline( source->tu, tokens, i ) ) {
          --i;
          break;
        }
        last_line = token_line;
      }

      token_kind = clang_getTokenKind( tokens[i] );
      if ( token_kind != CXToken_Identifier )
        continue;
      CXString const token_cxs =
        clang_getTokenSpelling( source->tu, tokens[i] );
      char const *const token_cs = clang_getCString( token_cxs );

      if ( strcmp( token_cs, "defined" ) != 0 &&
           strcmp( token_cs, "__has_include" ) != 0 ) {
        CXCursor const sym_csr = tidy_getCursorByName( token_cs, tu_csr );
        tidy_symbol *const sym = symbol_add( sym_csr, source );
        if ( sym != NULL )
          sym->is_root = true;
      }

      clang_disposeString( token_cxs );
    } // while
  } // for

  clang_disposeTokens( source->tu, tokens, token_count );
}

/**
 * Parses \a source and checks for errors.
 *
 * @note If there are errors, this function does not return.
 *
 * @param source The source file being tidied.
 */
static void source_parse_impl( tidy_source *source ) {
  assert( source != NULL );
  assert( source->argv_path != NULL );

  enum CXErrorCode const error_code = clang_parseTranslationUnit2(
    tidy_index,
    source->argv_path,
    source->ptu_argv->elements,
    STATIC_CAST( int, source->ptu_argv->len ),
    /*unsaved_files=*/NULL,
    /*num_unsaved_files=*/0,
    CXTranslationUnit_DetailedPreprocessingRecord,
    &source->tu
  );

  switch ( error_code ) {
    // LCOV_EXCL_START
    case CXError_ASTReadError:
      print_file_error( source->argv_path, 0, 0, "libclang AST error\n" );
      exit( EX_UNAVAILABLE );

    case CXError_Crashed:
      print_file_error( source->argv_path, 0, 0, "libclang crashed\n" );
      exit( EX_UNAVAILABLE );

    case CXError_InvalidArguments:
      print_error( "invalid arguments given to libclang\n" );
      exit( EX_SOFTWARE );
    // LCOV_EXCL_STOP

    case CXError_Failure:
      //
      // Libclang isn't specific about the cause of a failure, so see if the
      // reason is because the source file doesn't exist or isn't readable.
      //
      // Yes, this is TOCTAU (well, TAUTOC since we're checking after the
      // fact), but it's better than nothing.
      //
      if ( access( source->argv_path, R_OK ) == -1 ) {
        print_file_error( source->argv_path, 0, 0, "%s\n", STRERROR() );
        exit( EX_NOINPUT );
      }
      // LCOV_EXCL_START
      print_file_error( source->argv_path, 0, 0, "libclang failed\n" );
      exit( EX_DATAERR );
      // LCOV_EXCL_STOP

    case CXError_Success:
      //
      // All CXError_Success means is that libclang's parser didn't crash; it
      // doesn't mean the code is valid, so we have to check for diagnostics.
      //
      source_check_diagnostics( source );
      break;
  } // switch
}

/**
 * Cleans-up sources.
 */
static void sources_cleanup( void ) {
  ht_table_cleanup( &source_file_set, /*free_fn=*/NULL );
  if ( tidy_index != NULL )
    clang_disposeIndex( tidy_index );
}

////////// extern functions ///////////////////////////////////////////////////

bool is_source_path( char const *path ) {
  if ( path == NULL || path[0] == '\0' )
    return false;
  path = path_no_dot_slash( path );
  ht_entry_t const *const found_ht = ht_table_find( &source_file_set, path );
  return found_ht != NULL;
}

void source_cleanup( tidy_source *source ) {
  if ( source != NULL ) {
    // source->argv_path points to an argv so it doesn't need freeing
    if ( source->tu != NULL )
      clang_disposeTranslationUnit( source->tu );
  }
};

void source_parse( tidy_source *source ) {
  assert( source != NULL );
  source_parse_impl( source );
  CXFile const file = clang_getFile( source->tu, source->argv_path );
  source_cpp_visit( source, file );
  clang_getInclusions( source->tu, &include_visitor, source );
}

bool source_path_add( char const *path ) {
  assert( path != NULL );
  if ( config_path_is_ignored( path ) )
    return false;
  path = path_no_dot_slash( path );
  ht_insert_rv_t const hti = ht_table_insert(
    &source_file_set, CONST_CAST( char*, path ), strlen( path ) + 1/*'\0'*/
  );
  return hti.inserted;
}

void sources_init( void ) {
  ASSERT_RUN_ONCE();

  ht_table_init(
    &source_file_set, HT_DINT, 2.0, 256,
    POINTER_CAST( ht_cmp_fn_t, &strcmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

  tidy_index = clang_createIndex(
    /*excludeDeclarationsFromPCH=*/false,
    //
    // We only want errors printed and not warnings, so set this to false and
    // print errors ourselves.
    //
    /*displayDiagnostics=*/false
  );

  ATEXIT( &sources_cleanup );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
