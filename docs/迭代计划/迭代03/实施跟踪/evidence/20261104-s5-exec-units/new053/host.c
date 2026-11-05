#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "./libnew053plugin.dylib";
	int doClose = argc > 2 ? atoi(argv[2]) : 1;
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }
	int (*fn)(int) = (int (*)(int))dlsym(h, "new053_plugin_value");
	if (!fn) { fprintf(stderr, "dlsym failed\n"); return 3; }
	printf("value=%d\n", fn(20));
	if (doClose && dlclose(h) != 0) { fprintf(stderr, "dlclose failed\n"); return 4; }
	return 0;
}
