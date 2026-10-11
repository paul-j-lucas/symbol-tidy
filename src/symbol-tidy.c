/*
**      symbol-tidy -- Symbol tidier
**      src/symbol-tidy.c
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
 * Defines `main()`, a function for exit status, and global variables.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "symbol-tidy,h"
#include "color.h"
#include "hash_table.h"
#include "path_util,h"
#include "source.h"
#include "util.h"

#include <clang-c/Index.h>

// standard
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VisitorData VisitorData;

/* Visitor context passed down the AST traversal */
struct VisitorData {
  tidy_symbol *current_parent;
  hash_table_t *table;
};

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

char const   *prog_name;

/// @endcond

////////// local functions ////////////////////////////////////////////////////

tidy_symbol* get_or_create_node( hash_table_t *table, char const *usr ) {
  tidy_symbol ins_node = { .usr = (char*)usr };

  ht_insert_rv_t hti = ht_table_insert( table, &ins_node, sizeof ins_node );
  tidy_symbol *const node = HT_DINT( hti.entry );

  if ( hti.inserted )
    node->usr = strdup( usr );

  return node;
}

bool is_input_file( char const *file, int argc, char const **argv) {
  if ( file == NULL )
    return false;
  for ( int i = 1; i < argc; ++i ) {
    if ( strcmp( file, argv[i] ) == 0 )
      return true;
  } // for
  return false;
}

////////// extern functions ///////////////////////////////////////////////////

/**
 * The main entry point.
 *
 * @param argc The command-line argument count.
 * @param argv The command-line argument values.
 * @return Returns 0 on success, non-zero on failure.
 */
int main( int argc, char const *argv[] ) {
  prog_name = path_basename( argv[0] );

  if ( argc < 2 ) {
    fprintf(stderr, "Usage: %s <source_files...>\n", argv[0]);
    return 1;
  }

  colors_init();
  symbols_init();

  /* Parse all files */
  for ( int i = 1; i < argc; ++i ) {
    tidy_source source = {
      .path = argv[i],
      .lang = CXLanguage_C
    };
    source_init( &source, argc, argv );

    symbols_visit( &source );

    source_cleanup( &source );
  } // for

  symbols_mark_reachable();

  /* 2. Report unused/dead symbols via iterator */
  unsigned found_dead = 0;
  ht_iterator_init( &iter, &table );
  for ( tidy_symbol *sym; (sym = ht_iterator_next(&iter)) != NULL; ) {
    if ( n->is_tracked && !n->is_reachable ) {
      /* Ensure we only report symbols belonging to the files we specifically analyzed */
      if ( is_input_file( n->file, argc, argv ) ) {
        printf( "%s:%d:%d: unused symbol '%s'\n",
                n->file, n->line, n->col, n->name );
        ++found_dead;
      }
    }
  } // for

  if ( found_dead == 0 ) {
    printf( "No unused global symbols found.\n" );
  }

  return 0;
}
