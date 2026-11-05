/* Minimal instrumented module: its own copy of the LLVM profile runtime is
   linked in by -fprofile-instr-generate, exactly like each coverage dylib. */
int new053_plugin_value(int x) { return x * 2 + 1; }
