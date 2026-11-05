#!/bin/bash
# compiler launcher: put physical Boost copy first in the include search order
c="$1"; shift
exec "$c" -I/tmp/wt-i03-reverify-20261103/boost-include "$@"
