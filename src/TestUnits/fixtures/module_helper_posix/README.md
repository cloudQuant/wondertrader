# POSIX module path focused checks

Run `src/TestUnits/fixtures/module_helper_posix/run.sh /tmp/module-helper-posix-build` from the repository root. The script builds into the supplied directory and tests mocked `dladdr` failure, null and empty names, absolute and slash-free names, concurrent first access, and real executable and dynamic-plugin lookup. It requires a C++17 compiler, pthreads and Perl; it does not create symbolic links. Each mock mode runs in a fresh process because the helper caches its first result.

On failure, the helper permanently caches an empty result for the lifetime of that translation unit. Callers must treat an empty directory as failure rather than silently loading a relative path. These focused tests do not validate all consumers, Windows or arbitrary platforms.
