#!/bin/sh

autoreconf -i

echo "patching m4/libtool.m4: See https://github.com/gperftools/gperftools/issues/1429#issuecomment-1794976863"

(set -x; patch --forward -t --reject-file=- m4/libtool.m4 m4/libtool.patch && autoreconf -i)
