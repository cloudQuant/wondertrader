/* R20_wtbtcore#1 extended: levels 1..19, several sizes, one-shot and reused context; prints a digest of every frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"
static unsigned long long fnv(const unsigned char* p, size_t n){unsigned long long h=1469598103934665603ULL;for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;}return h;}
int main(void)
{
	const size_t sizes[] = { 0, 1, 88, 4096, 200000 };
	unsigned long long digest = 0;
	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	for (size_t si = 0; si < sizeof(sizes)/sizeof(sizes[0]); ++si) {
		size_t n = sizes[si];
		unsigned char* src = (unsigned char*)malloc(n ? n : 1);
		for (size_t i = 0; i < n; ++i) src[i] = (unsigned char)((i * 2654435761u) >> (i % 7)) ^ (unsigned char)(i / 97);
		size_t cap = ZSTD_compressBound(n);
		unsigned char* dst = (unsigned char*)malloc(cap);
		unsigned char* back = (unsigned char*)malloc(n ? n : 1);
		for (int level = 1; level <= 19; ++level) {
			size_t c = ZSTD_compressCCtx(cctx, dst, cap, src, n, level);
			if (ZSTD_isError(c)) { printf("compress error n=%zu l=%d\n", n, level); return 2; }
			size_t c2 = ZSTD_compress(dst, cap, src, n, level);
			if (ZSTD_isError(c2) || c2 != c) { printf("one-shot differs n=%zu l=%d\n", n, level); return 3; }
			size_t d = ZSTD_decompress(back, n ? n : 1, dst, c);
			if (ZSTD_isError(d) || d != n || (n && memcmp(back, src, n))) { printf("roundtrip n=%zu l=%d\n", n, level); return 4; }
			digest ^= fnv(dst, c) + (unsigned long long)level * 1315423911ULL + n;
		}
		free(src); free(dst); free(back);
	}
	ZSTD_freeCCtx(cctx);
	printf("all ok digest=%016llx\n", digest);
	return 0;
}
