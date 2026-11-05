#!/bin/bash
# LLVM22 launcher: physical Boost first; pre-include stdlib.h (Homebrew fmt needs malloc/free with libc++ 22)
c="$1"; shift
exec "$c" -I/tmp/wt-i03-reverify-20261103/boost-include -include stdlib.h "$@"
