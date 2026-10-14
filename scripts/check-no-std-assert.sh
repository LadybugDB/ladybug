#!/bin/sh
# Vendored code is not ours to police (e.g. the SQLite amalgamation and
# NetworKit headers legitimately include <assert.h>/<cassert>).
! grep --color=auto -r --exclude-dir=third_party --exclude-dir=vendor \
    "include <assert.h>\|include <cassert>" $1
