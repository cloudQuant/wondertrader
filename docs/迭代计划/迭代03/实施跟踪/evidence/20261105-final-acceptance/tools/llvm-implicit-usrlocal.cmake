# Sanitizer evidence builds with Homebrew LLVM only (not part of the product tree).
# AppleClang treats /usr/local/include as an implicit include dir, so CMake never emits
# -I/usr/local/include and the coverage Boost bridge (-I) wins over the Boost 1.81 headers
# installed there. Homebrew LLVM does not, so CMake would emit -I/usr/local/include ahead of
# the bridge and compile against Boost 1.81 while linking Homebrew Boost 1.88. Mark the dir
# implicit (CMake drops the -I) and re-add it via -isystem in the flags, after the bridge.
list(APPEND CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES /usr/local/include)
list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES /usr/local/include)
