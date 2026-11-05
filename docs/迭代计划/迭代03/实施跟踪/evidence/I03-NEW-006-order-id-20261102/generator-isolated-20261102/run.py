#!/usr/bin/env python
"""Compile each unmodified generator body with a deterministic fake clock."""
import hashlib
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[7]
SOURCES = ["src/WtCore/TraderAdapter.cpp", "src/WtUftCore/TraderAdapter.cpp", "src/WtBtCore/HftMocker.cpp"]
PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
struct TimeUtils {
    static bool fail;
    static int64_t now;
    static uint32_t getCurDate() { return now == 85899345000LL ? 19720921 : 20240101; }
    static int64_t getLocalTimeNow() { return now; }
    static int64_t makeTime(uint32_t, int) { return fail ? 0 : 1704067200000LL; }
};
bool TimeUtils::fail = false;
int64_t TimeUtils::now = 1704067201000LL;
'''
SUFFIX = r'''
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "healthy")) {
        auto id = makeLocalOrderID();
        std::printf("healthy_first=%u\n", id);
        return id == 50 ? 0 : 1;
    }
    if (!std::strcmp(argv[1], "retry")) {
        TimeUtils::fail = true;
        auto failed = makeLocalOrderID();
        TimeUtils::fail = false;
        auto recovered = makeLocalOrderID();
        std::printf("mktime_failure_id=%u after_recovery_id=%u healthy_first=50\n", failed, recovered);
        return failed != 0 && recovered == failed + 1 && recovered != 50 ? 0 : 1;
    }
    if (!std::strcmp(argv[1], "wrap")) {
        TimeUtils::fail = true;
        TimeUtils::now = 85899345000LL;
        auto first = makeLocalOrderID();
        for (int i = 0; i < 44; ++i) makeLocalOrderID();
        auto reserved_failure = makeLocalOrderID();
        auto reserved_zero = makeLocalOrderID();
        auto reused = makeLocalOrderID();
        std::printf("first=%u failure_sentinel=%u no_order_sentinel=%u next=%u\n", first, reserved_failure, reserved_zero, reused);
        return first == UINT32_MAX - 45 && reserved_failure == UINT32_MAX && reserved_zero == 0 && reused == 1 ? 0 : 1;
    }
    if (!std::strcmp(argv[1], "concurrent")) {
        constexpr int threads = 12, count = 1000;
        std::vector<uint32_t> ids(threads * count);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] { for (int i = 0; i < count; ++i) ids[t * count + i] = makeLocalOrderID(); });
        for (auto& thread : pool) thread.join();
        std::sort(ids.begin(), ids.end());
        for (int i = 0; i < threads * count; ++i) if (ids[i] != 50u + i) return 1;
        std::printf("threads=%d ids=%zu first=%u last=%u unique=yes\n", threads, ids.size(), ids.front(), ids.back());
        return 0;
    }
    return 2;
}
'''

with tempfile.TemporaryDirectory(prefix="i03-order-generator-") as tmp:
    for rel in SOURCES:
        raw = (ROOT / rel).read_bytes()
        src = raw.decode()
        marker = "uint32_t makeLocalOrderID()"
        start = src.index(marker)
        brace = src.index("{", start)
        depth = 1
        end = brace + 1
        while depth:
            depth += (src[end] == "{") - (src[end] == "}")
            end += 1
        body = src[start:end]
        generated = pathlib.Path(tmp) / "generator.cpp"
        binary = pathlib.Path(tmp) / "generator"
        generated.write_text(PREFIX + body + SUFFIX)
        print(f"SOURCE {rel} sha256={hashlib.sha256(raw).hexdigest()}", flush=True)
        subprocess.run(["c++", "-std=c++17", "-O1", "-pthread", str(generated), "-o", str(binary)], check=True)
        for mode in ("healthy", "retry", "wrap", "concurrent"):
            print(f"  {mode}: ", end="", flush=True)
            subprocess.run([str(binary), mode], check=True)
