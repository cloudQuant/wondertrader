// R23-006: A separate process holds a mapping made before writer expansion.
// This is a known-red contract probe, not a reader cache/slice test.
#include "../../coverage/r23_storage4/r23_writer_fixture.h"
#include "../../coverage/p10_storage_writer/p10_writer_test_hooks.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

// Consulted only by a separately instrumented source snapshot, never by the
// production translation unit or builds without the dedicated probe.
static bool fail_truncate_once = false;
extern "C" bool wt_r23_fail_truncate_once() noexcept {
    const bool fail = fail_truncate_once;
    fail_truncate_once = false;
    return fail;
}

namespace {

struct Observation {
    uint64_t mapped_bytes;
    uint64_t required_bytes;
    uint32_t observed_capacity;
    uint32_t observed_size;
    double old_last_volume;
    double remapped_new_volume;
};

bool transfer(int fd, void* data, size_t length, bool writing) {
    auto* ptr = static_cast<char*>(data);
    while (length) {
        ssize_t n = writing ? ::write(fd, ptr, length) : ::read(fd, ptr, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        ptr += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

struct Child {
    pid_t pid = -1;
    ~Child() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
};

struct ShortWriteInterleave {
    int request = -1;
    int ack = -1;
    bool invoked = false;
};

struct RemapFailure {
    bool pending = true;
    bool retained = false;
};

bool failRemapOnce(void* context, bool retained) noexcept {
    auto* state = static_cast<RemapFailure*>(context);
    if (!state || !state->pending) return false;
    state->pending = false;
    state->retained = retained;
    return true;
}

bool appendWithOtherWriter(void* context, const char* path, uint64_t oldSize,
                           uint64_t newSize) noexcept {
    auto* state = static_cast<ShortWriteInterleave*>(context);
    if (!state || !path || newSize <= oldSize + 64) return false;
    const int fd = ::open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return false;
    char partial[16]{};
    bool ok = transfer(fd, partial, sizeof(partial), true);
    ::close(fd);
    char token = 'W';
    ok = ok && transfer(state->request, &token, 1, true) &&
         transfer(state->ack, &token, 1, false) && token == 'A';
    state->invoked = ok;
    return ok;
}

bool appendThenFailRollback(void* context, const char* path, uint64_t oldSize,
                            uint64_t newSize) noexcept {
    auto* called = static_cast<bool*>(context);
    if (!called || *called || !path || newSize <= oldSize + 16) return false;
    int fd = ::open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return false;
    char partial[16]{};
    const bool ok = transfer(fd, partial, sizeof(partial), true);
    ::close(fd);
    if (ok) { *called = true; fail_truncate_once = true; }
    return ok;
}

struct HookCleanup {
    WtDataWriter* writer;
    ~HookCleanup() { p10_writer_test_hooks::uninstall(writer); }
};

class R23OldMapping : public r23::R23WriterTest {};

TEST_F(R23OldMapping, OldViewMustNotExposeRecordsBeyondItsMappedLength) {
    using namespace r23;
    auto* writer = make_writer("\"groupsize\":100000");
    ASSERT_NE(writer, nullptr);
    TickSpec tick;
    tick.trading_date = tick.action_date = 20240108;
    for (uint32_t i = 0; i < 2500; ++i) {
        tick.action_time = 90000000u + i * 1000u;
        tick.total_volume = 1000.0 + i;
        write_tick(writer, tick);
    }
    const std::string path = rt_tick_file("rb2210");
    const auto before = read_block_header<RTDayBlockHeader>(path);
    ASSERT_EQ(before._capacity, 2500u);
    ASSERT_EQ(before._size, 2500u);

    int toChild[2], fromChild[2];
    ASSERT_EQ(::pipe(toChild), 0);
    ASSERT_EQ(::pipe(fromChild), 0);
    Child child;
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if (child.pid == 0) {
        // Only simple system calls are made in the post-fork child; the writer
        // already owns background threads. Parent retains all owning objects.
        ::alarm(15);
        ::close(toChild[1]); ::close(fromChild[0]);
        int fd = ::open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || ::fstat(fd, &st) || st.st_size <= 0) ::_exit(10);
        const size_t oldLen = static_cast<size_t>(st.st_size);
        void* map = ::mmap(nullptr, oldLen, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) ::_exit(11);
        RTDayBlockHeader old{};
        std::memcpy(&old, map, sizeof(old));
        if (old._capacity != 2500u || old._size != 2500u) ::_exit(12);
        char token = 'R';
        if (!transfer(fromChild[1], &token, 1, true) || !transfer(toChild[0], &token, 1, false)) ::_exit(13);
        RTDayBlockHeader now{};
        std::memcpy(&now, map, sizeof(now));
        Observation o{};
        o.mapped_bytes = oldLen;
        o.required_bytes = sizeof(RTDayBlockHeader) + uint64_t(now._size) * sizeof(WTSTickStruct);
        o.observed_capacity = now._capacity;
        o.observed_size = now._size;
        WTSTickStruct last{};
        const size_t lastOffset = sizeof(RTDayBlockHeader) + 2499u * sizeof(last);
        if (lastOffset + sizeof(last) > oldLen) ::_exit(14);
        std::memcpy(&last, static_cast<const char*>(map) + lastOffset, sizeof(last));
        o.old_last_volume = last.total_volume;
        if (::fstat(fd, &st) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) < o.required_bytes) ::_exit(15);
        const size_t newLen = static_cast<size_t>(st.st_size);
        void* fresh = ::mmap(nullptr, newLen, PROT_READ, MAP_SHARED, fd, 0);
        if (fresh == MAP_FAILED) ::_exit(16);
        WTSTickStruct added{};
        std::memcpy(&added, static_cast<const char*>(fresh) + sizeof(RTDayBlockHeader) + 2500u * sizeof(added), sizeof(added));
        o.remapped_new_volume = added.total_volume;
        if (!transfer(fromChild[1], &o, sizeof(o), true)) ::_exit(17);
        ::munmap(fresh, newLen); ::munmap(map, oldLen); ::close(fd); ::_exit(0);
    }
    ::close(toChild[0]); ::close(fromChild[1]);
    pollfd ready{fromChild[0], POLLIN, 0};
    ASSERT_EQ(::poll(&ready, 1, 10000), 1);
    char token = 0;
    ASSERT_TRUE(transfer(fromChild[0], &token, 1, false));
    ASSERT_EQ(token, 'R');
    tick.action_time = 90000000u + 2500u * 1000u;
    tick.total_volume = 3500.0;
    write_tick(writer, tick);
    token = 'G';
    ASSERT_TRUE(transfer(toChild[1], &token, 1, true));
    ready.revents = 0;
    ASSERT_EQ(::poll(&ready, 1, 10000), 1);
    Observation observed{};
    ASSERT_TRUE(transfer(fromChild[0], &observed, sizeof(observed), false));
    int status = 0;
    ASSERT_EQ(::waitpid(child.pid, &status, 0), child.pid);
    child.pid = -1;
    ::close(toChild[1]); ::close(fromChild[0]);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(observed.observed_capacity, 5000u);
    EXPECT_EQ(observed.observed_size, 2501u);
    EXPECT_DOUBLE_EQ(observed.old_last_volume, 3499.0);
    EXPECT_DOUBLE_EQ(observed.remapped_new_volume, 3500.0);
    // If a reader checked capacity just before the growth, its retained view
    // is too short for the subsequently published count. Never dereference it.
    EXPECT_LE(observed.required_bytes, observed.mapped_bytes)
        << "published count exceeds independently retained mapping";
}

// Tests the documented unresolved concurrent-writer case. A second process
// appends after a simulated partial extension and before rollback. No claim is
// made that the legacy storage format supports multiple concurrent writers.
TEST_F(R23OldMapping, RollbackMustNotTruncateAnotherProcessesAppend) {
    using namespace r23;
    auto* writer = make_writer("\"groupsize\":100000");
    ASSERT_NE(writer, nullptr);
    TickSpec tick;
    tick.trading_date = tick.action_date = 20240108;
    for (uint32_t i = 0; i < 2500; ++i) {
        tick.action_time = 90000000u + i * 1000u;
        tick.total_volume = 1000.0 + i;
        write_tick(writer, tick);
    }
    const std::string path = rt_tick_file("rb2210");
    const auto before = read_block_header<RTDayBlockHeader>(path);
    const uint64_t beforeSize = file_size(path);
    ASSERT_EQ(before._capacity, 2500u);
    ASSERT_EQ(before._size, 2500u);

    int toChild[2], fromChild[2];
    ASSERT_EQ(::pipe(toChild), 0);
    ASSERT_EQ(::pipe(fromChild), 0);
    Child child;
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if (child.pid == 0) {
        ::alarm(15);
        ::close(toChild[1]); ::close(fromChild[0]);
        char token = 0;
        if (!transfer(toChild[0], &token, 1, false) || token != 'W') ::_exit(20);
        int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        if (fd < 0) ::_exit(21);
        char marker[] = "external-writer";
        const bool ok = transfer(fd, marker, sizeof(marker), true);
        ::close(fd);
        token = 'A';
        if (!ok || !transfer(fromChild[1], &token, 1, true)) ::_exit(22);
        ::_exit(0);
    }
    ::close(toChild[0]); ::close(fromChild[1]);
    ShortWriteInterleave interleave{toChild[1], fromChild[0], false};
    p10_writer_test_hooks::Hooks hooks;
    hooks.context = &interleave;
    hooks.simulate_next_rt_resize_short_write = appendWithOtherWriter;
    ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
    HookCleanup cleanup{writer};
    tick.action_time = 90000000u + 2500u * 1000u;
    tick.total_volume = 3500.0;
    write_tick(writer, tick);
    ASSERT_TRUE(interleave.invoked);
    int status = 0;
    ASSERT_EQ(::waitpid(child.pid, &status, 0), child.pid);
    child.pid = -1;
    ::close(toChild[1]); ::close(fromChild[0]);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    const auto after = read_block_header<RTDayBlockHeader>(path);
    EXPECT_EQ(after._size, before._size);
    EXPECT_EQ(after._capacity, before._capacity);
    WTSTickStruct old{};
    ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(path, 2499, old)));
    EXPECT_DOUBLE_EQ(old.total_volume, 3499.0);
    EXPECT_GT(file_size(path), beforeSize)
        << "rollback truncated bytes appended by another process";
}

TEST_F(R23OldMapping, FailedRemapPreservesOldProcessViewAndRetryCanRemap) {
    using namespace r23;
    auto* writer = make_writer("\"groupsize\":100000");
    ASSERT_NE(writer, nullptr);
    TickSpec tick;
    tick.trading_date = tick.action_date = 20240108;
    for (uint32_t i = 0; i < 2500; ++i) {
        tick.action_time = 90000000u + i * 1000u;
        tick.total_volume = 1000.0 + i;
        write_tick(writer, tick);
    }
    const std::string path = rt_tick_file("rb2210");
    int toChild[2], fromChild[2];
    ASSERT_EQ(::pipe(toChild), 0);
    ASSERT_EQ(::pipe(fromChild), 0);
    Child child;
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if (child.pid == 0) {
        ::alarm(15);
        ::close(toChild[1]); ::close(fromChild[0]);
        const int fd = ::open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || ::fstat(fd, &st) || st.st_size <= 0) ::_exit(30);
        const size_t length = static_cast<size_t>(st.st_size);
        void* view = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) ::_exit(31);
        RTDayBlockHeader first{};
        std::memcpy(&first, view, sizeof(first));
        if (first._capacity != 2500u || first._size != 2500u) ::_exit(32);
        char token = 'R';
        if (!transfer(fromChild[1], &token, 1, true) || !transfer(toChild[0], &token, 1, false) || token != 'F') ::_exit(33);
        RTDayBlockHeader failed{};
        std::memcpy(&failed, view, sizeof(failed));
        WTSTickStruct old{};
        const size_t oldOffset = sizeof(RTDayBlockHeader) + 2499u * sizeof(old);
        if (oldOffset + sizeof(old) > length) ::_exit(34);
        std::memcpy(&old, static_cast<const char*>(view) + oldOffset, sizeof(old));
        if (failed._capacity != 2500u || failed._size != 2500u || old.total_volume != 3499.0) ::_exit(35);
        token = 'A';
        if (!transfer(fromChild[1], &token, 1, true) || !transfer(toChild[0], &token, 1, false) || token != 'G') ::_exit(36);
        if (::fstat(fd, &st) || st.st_size <= 0) ::_exit(37);
        const size_t expandedLength = static_cast<size_t>(st.st_size);
        void* fresh = ::mmap(nullptr, expandedLength, PROT_READ, MAP_SHARED, fd, 0);
        if (fresh == MAP_FAILED) ::_exit(38);
        RTDayBlockHeader retried{};
        std::memcpy(&retried, fresh, sizeof(retried));
        const size_t newOffset = sizeof(RTDayBlockHeader) + 2500u * sizeof(old);
        if (retried._capacity != 5000u || retried._size != 2501u || newOffset + sizeof(old) > expandedLength) ::_exit(39);
        std::memcpy(&old, static_cast<const char*>(fresh) + newOffset, sizeof(old));
        if (old.total_volume != 3501.0) ::_exit(40);
        token = 'S';
        if (!transfer(fromChild[1], &token, 1, true)) ::_exit(41);
        ::munmap(fresh, expandedLength); ::munmap(view, length); ::close(fd); ::_exit(0);
    }
    ::close(toChild[0]); ::close(fromChild[1]);
    pollfd ready{fromChild[0], POLLIN, 0};
    ASSERT_EQ(::poll(&ready, 1, 10000), 1);
    char token = 0;
    ASSERT_TRUE(transfer(fromChild[0], &token, 1, false));
    ASSERT_EQ(token, 'R');
    RemapFailure failure;
    p10_writer_test_hooks::Hooks hooks;
    hooks.context = &failure;
    hooks.fail_next_rt_resize_map = failRemapOnce;
    ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
    HookCleanup cleanup{writer};
    tick.action_time = 90000000u + 2500u * 1000u;
    tick.total_volume = 3500.0;
    write_tick(writer, tick);
    ASSERT_FALSE(failure.pending);
    ASSERT_TRUE(failure.retained);
    token = 'F';
    ASSERT_TRUE(transfer(toChild[1], &token, 1, true));
    ready.revents = 0;
    ASSERT_EQ(::poll(&ready, 1, 10000), 1);
    ASSERT_TRUE(transfer(fromChild[0], &token, 1, false));
    ASSERT_EQ(token, 'A');
    tick.action_time += 1000u;
    tick.total_volume += 1.0;
    write_tick(writer, tick);
    token = 'G';
    ASSERT_TRUE(transfer(toChild[1], &token, 1, true));
    ready.revents = 0;
    ASSERT_EQ(::poll(&ready, 1, 10000), 1);
    ASSERT_TRUE(transfer(fromChild[0], &token, 1, false));
    ASSERT_EQ(token, 'S');
    int status = 0;
    ASSERT_EQ(::waitpid(child.pid, &status, 0), child.pid);
    child.pid = -1;
    ::close(toChild[1]); ::close(fromChild[0]);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(R23OldMapping, FailedRollbackKeepsPublishedCapacityAndAllowsRetry) {
    using namespace r23;
    auto* writer = make_writer("\"groupsize\":100000");
    ASSERT_NE(writer, nullptr);
    TickSpec tick;
    tick.trading_date = tick.action_date = 20240108;
    for (uint32_t i = 0; i < 2500; ++i) {
        tick.action_time = 90000000u + i * 1000u;
        tick.total_volume = 1000.0 + i;
        write_tick(writer, tick);
    }
    const std::string path = rt_tick_file("rb2210");
    const auto before = read_block_header<RTDayBlockHeader>(path);
    const uint64_t beforeSize = file_size(path);
    ASSERT_EQ(before._capacity, 2500u);
    ASSERT_EQ(before._size, 2500u);
    bool injected = false;
    p10_writer_test_hooks::Hooks hooks;
    hooks.context = &injected;
    hooks.simulate_next_rt_resize_short_write = appendThenFailRollback;
    ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
    HookCleanup cleanup{writer};
    tick.action_time = 90000000u + 2500u * 1000u;
    tick.total_volume = 3500.0;
    write_tick(writer, tick);
    ASSERT_TRUE(injected);
    EXPECT_FALSE(fail_truncate_once) << "snapshot did not inject truncate failure";
    EXPECT_TRUE(_sink->has_log("Failed to roll back partial RT cache extension", LL_ERROR));
    auto failed = read_block_header<RTDayBlockHeader>(path);
    EXPECT_EQ(failed._size, before._size);
    EXPECT_EQ(failed._capacity, before._capacity);
    EXPECT_EQ(file_size(path), beforeSize + 16);
    WTSTickStruct preserved{};
    ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(path, 2499, preserved)));
    EXPECT_DOUBLE_EQ(preserved.total_volume, 3499.0);

    tick.action_time += 1000u;
    tick.total_volume += 1.0;
    write_tick(writer, tick);
    const auto retried = read_block_header<RTDayBlockHeader>(path);
    EXPECT_EQ(retried._capacity, 5000u);
    EXPECT_EQ(retried._size, 2501u);
    WTSTickStruct added{};
    ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(path, 2500, added)));
    EXPECT_DOUBLE_EQ(added.total_volume, 3501.0);
}

} // namespace
