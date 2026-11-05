#include "../../../Includes/WTSStruct.h"
#include "../../../WTSUtils/WTSCmpHelper.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    wtp::WTSBarStruct bar{};
    bar.date = 20240102;
    bar.open = 100;
    bar.close = 101;
    const size_t inputSize = sizeof(bar);
    const size_t capacity = ZSTD_compressBound(inputSize);
    std::printf("mode=%s source_nonnull=%d source_size=%zu dest_capacity=%zu zstd=%u\n",
                argv[1], static_cast<const void*>(&bar) != nullptr, inputSize,
                capacity, ZSTD_versionNumber());
    std::fflush(stdout);
    if (std::strcmp(argv[1], "direct") == 0) {
        std::vector<char> dst(capacity);
        const size_t size = ZSTD_compress(dst.data(), dst.size(), &bar, inputSize, 1);
        if (ZSTD_isError(size)) return 3;
        wtp::WTSBarStruct recovered{};
        const size_t actual = ZSTD_decompress(&recovered, sizeof(recovered), dst.data(), size);
        if (ZSTD_isError(actual) || actual != sizeof(bar) || std::memcmp(&bar, &recovered, sizeof(bar))) return 4;
        std::puts("direct roundtrip PASS");
        return 0;
    }
    if (std::strcmp(argv[1], "wrapper") == 0) {
        const std::string compressed = WTSCmpHelper::compress_data(&bar, inputSize);
        const std::string recovered = WTSCmpHelper::uncompress_data(compressed.data(), compressed.size());
        if (recovered.size() != sizeof(bar) || std::memcmp(&bar, recovered.data(), sizeof(bar))) return 5;
        std::puts("wrapper roundtrip PASS");
        return 0;
    }
    if (std::strcmp(argv[1], "sweep") == 0) {
        // Levels 1..19 select the fast, double-fast, lazy and optimal strategies, which
        // were all part of the 2026-11-05 null-pointer arithmetic fix; one-shot calls and
        // a reused context must round-trip byte for byte.
        const size_t sizes[] = {0, 1, 88, 4096, 200000};
        ZSTD_CCtx* reused = ZSTD_createCCtx();
        if (!reused) return 6;
        unsigned checked = 0;
        for (int level = 1; level <= 19; ++level) {
            for (size_t size : sizes) {
                std::vector<char> input(size);
                for (size_t i = 0; i < size; ++i) input[i] = static_cast<char>((i * 131u + static_cast<size_t>(level)) % 251u);
                std::vector<char> dst(ZSTD_compressBound(size));
                for (int pass = 0; pass < 2; ++pass) {
                    const size_t csize = pass == 0
                        ? ZSTD_compress(dst.data(), dst.size(), size ? input.data() : nullptr, size, level)
                        : ZSTD_compressCCtx(reused, dst.data(), dst.size(), size ? input.data() : nullptr, size, level);
                    if (ZSTD_isError(csize)) { ZSTD_freeCCtx(reused); return 7; }
                    std::vector<char> back(size ? size : 1);
                    const size_t dsize = ZSTD_decompress(back.data(), size, dst.data(), csize);
                    if (ZSTD_isError(dsize) || dsize != size || (size && std::memcmp(back.data(), input.data(), size))) {
                        ZSTD_freeCCtx(reused); return 8;
                    }
                    ++checked;
                }
            }
        }
        ZSTD_freeCCtx(reused);
        std::printf("sweep roundtrip PASS cases=%u\n", checked);
        return 0;
    }
    return 2;
}
