/* R20_wtbtcore#1: compress/decompress an 88-byte buffer through the vendored zstd under UBSan. */
#include <stdio.h>
#include <string.h>
#include "zstd.h"
int main(void)
{
	unsigned char src[88];
	for (int i = 0; i < 88; ++i) src[i] = (unsigned char)(i * 7 + 3);
	unsigned char dst[512];
	unsigned char back[88];
	for (int level = 1; level <= 3; ++level) {
		size_t c = ZSTD_compress(dst, sizeof(dst), src, sizeof(src), level);
		if (ZSTD_isError(c)) { printf("compress error %s\n", ZSTD_getErrorName(c)); return 2; }
		size_t d = ZSTD_decompress(back, sizeof(back), dst, c);
		if (ZSTD_isError(d) || d != sizeof(src) || memcmp(back, src, sizeof(src)) != 0) { printf("roundtrip mismatch\n"); return 3; }
		printf("level %d: %zu -> %zu -> %zu ok\n", level, sizeof(src), c, d);
	}
	/* Reusing one context exercises the same reset path. */
	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	for (int round = 0; round < 3; ++round) {
		size_t c = ZSTD_compressCCtx(cctx, dst, sizeof(dst), src, sizeof(src), 1);
		if (ZSTD_isError(c)) { printf("cctx error\n"); return 4; }
	}
	ZSTD_freeCCtx(cctx);
	printf("done\n");
	return 0;
}
