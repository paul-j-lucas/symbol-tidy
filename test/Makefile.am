##
#	symbol-tidy -- Symbol tidier
#	test/Makefile.am
#
#	Copyright (C) 2026  Paul J. Lucas
#
#	This program is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	(at your option) any later version.
#
#	This program is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
#	You should have received a copy of the GNU General Public License
#	along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

# Suppress printing of "--- target ---" when -j is given to BSD's make.
.MAKE.JOB.PREFIX=

AUTOMAKE_OPTIONS = 1.12			# needed for TEST_LOG_DRIVER

###############################################################################

# Basic sanity tests
TESTS=	tests/hello.test

###############################################################################

ABS_TOP_BUILDDIR =	$(abs_top_builddir)
EXTRA_DIST =		bin data expected tests
TEST_EXTENSIONS =	.test
TEST_LOG_DRIVER =	$(srcdir)/bin/run_test.sh
TOP_SRCDIR =		$(top_srcdir)
UPDATE_TEST =		$(srcdir)/bin/update_test.sh

AM_TESTS_ENVIRONMENT = TOP_SRCDIR=$(TOP_SRCDIR) ABS_TOP_BUILDDIR=$(ABS_TOP_BUILDDIR) ; export TOP_SRCDIR ABS_TOP_BUILDDIR ;

.PHONY: top-clean-coverage

top-clean-coverage:
if ENABLE_COVERAGE
	cd $(top_builddir) && $(MAKE) $(AM_MAKEFLAGS) clean-coverage
endif

dist-hook:
	cd $(distdir)/tests && rm -f *.log *.trs

update: top-clean-coverage
	@ls $(TESTS) | xargs -n255 -r \
	$(UPDATE_TEST) -B "$(ABS_TOP_BUILDDIR)" -s "$(TOP_SRCDIR)"

# vim:set noet sw=8 ts=8:
