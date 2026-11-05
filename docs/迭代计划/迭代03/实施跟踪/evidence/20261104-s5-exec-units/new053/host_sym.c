#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv)
{
	void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) return 2;
	void* fn = dlsym(h, "new053_plugin_value");
	Dl_info info;
	if (!fn || !dladdr(fn, &info)) return 3;
	printf("module_base=%p\n", info.dli_fbase);
	fflush(stdout);
	return dlclose(h) == 0 ? 0 : 4;
}
