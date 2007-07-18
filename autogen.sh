#!/bin/sh

# Before using, you should figure out all the .m4 macros that your
# configure.m4 script needs and make sure they exist in the autoconf/
# directory.
#
# These are the files that this script might edit:
#    aclocal.m4 configure Makefile.in src/config.h.in \
#    depcomp config.guess config.sub install-sh missing mkinstalldirs \
#    ltmain.sh
#
# Here's a command you can run to see what files aclocal will import:
#  aclocal -I ../autoconf --output=- | sed -n 's/^m4_include..\([^]]*\).*/\1/p'

set -ex
rm -rf autom4te.cache

aclocal --force -I m4
grep -q LIBTOOL configure.ac && libtoolize -c -f
autoconf -f -W all,no-obsolete
autoheader -f -W all
automake -a -c -f -W all,no-override

rm -rf autom4te.cache
exit 0
