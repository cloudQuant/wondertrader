#pragma once

// Offline staging only: does not stop writers, verify evidence provenance or publish V2.
#include "LMDBBarKeyV2.hpp"
#include "../WTSUtils/lmdb/lmdb.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace wt_ad_bar_migration
{
using namespace wt_ad_bar_key_v2;
constexpr const char* stageName = "wt.minute.bar.v2.staging";
constexpr const char* metadataName = "wt.minute.bar.v2.metadata";
constexpr std::size_t maxBatchRecords = 4096;
constexpr const char* evidenceName = "wt.minute.bar.v2.evidence";

// Caller must source this evidence independently of the V1 key AND value.
struct Evidence
{
    std::string exchange;
    std::string code;
    std::uint64_t verifiedTime = 0;
    bool independentlyVerified = false;
};
using EvidenceLookup = std::function<bool(const MDB_val&, const MDB_val&, Evidence&)>;
struct EvidenceInput { MDB_val key; MDB_val value; Evidence proof; };

enum class ResultCode
{
    Prepared, Complete, Incomplete, InvalidArgument, UnpreparedBindings,
    UnsupportedPlatform, SameEnvironment,
    UnsafeEnvironment, SourceError, SourceChanged, SourceInconsistent,
    DestinationError, DestinationChanged, ReentrantCall,
    UnsupportedDatabaseFlags, MissingEvidence, InvalidRecord, Conflict,
    UnexpectedStagingContents, MetadataMismatch, CallbackError, EvidenceCapabilityRequired
};
struct Result
{
    ResultCode code = ResultCode::InvalidArgument;
    int lmdbError = 0;
    Status codecStatus = Status::Ok;
    std::uint64_t committedRecords = 0; // Meaningful for Complete/Incomplete only.
};

// A single noncopyable binding owns this component's synchronization for one
// destination. prepareBindings must run while BOTH environments are exclusive
// to the caller, before any worker starts. The caller must create only one
// binding per destination, route all destination writes through it, and keep
// environments/DBIs alive until all calls and callbacks complete. No DBI open,
// DBI close, env close/resize, or uncoordinated target write may overlap calls.
// These ownership rules cannot be enforced against unrelated code/processes.
class Bindings
{
public:
    Bindings() = default;
    Bindings(const Bindings&) = delete;
    Bindings& operator=(const Bindings&) = delete;
    MDB_env* destination() const noexcept { return ready_.load(std::memory_order_acquire) ? dest_ : nullptr; }
    MDB_dbi stageDbi() const noexcept { return ready_.load(std::memory_order_acquire) ? stageDb_ : 0; }
    // Stores caller-supplied claims; does not authenticate their issuer or
    // observe out-of-band revocation. All updates must use this capability.
    bool putEvidence(const MDB_val&, const MDB_val&, const Evidence&);
    bool putEvidenceBatch(const std::vector<EvidenceInput>&);
    bool revokeEvidence(const MDB_val&);
private:
    friend Result prepareBindings(MDB_env*, MDB_env*, Bindings&);
    friend Result stageBatch(const Bindings&, LegacySourceHost, LegacyValueLayout,
                             const EvidenceLookup&, std::size_t);
    friend Result stageBatch(const Bindings&, LegacySourceHost, LegacyValueLayout, std::size_t);
    static Result stageBatchInternal(const Bindings&, LegacySourceHost, LegacyValueLayout,
                                     const EvidenceLookup&, std::size_t);
    MDB_env* source_ = nullptr;
    MDB_env* dest_ = nullptr;
    MDB_dbi sourceDb_ = 0, stageDb_ = 0, metaDb_ = 0, evidenceDb_ = 0;
    std::atomic<bool> ready_{false};
    mutable std::mutex writerMutex_;
    mutable std::mutex evidenceMutex_;
};

namespace detail
{
inline thread_local bool inStageCallback = false;
struct ReentryGuard
{
    ReentryGuard() { inStageCallback = true; }
    ~ReentryGuard() { inStageCallback = false; }
};
struct Transaction
{
    MDB_txn* ptr = nullptr;
    ~Transaction() { if (ptr) mdb_txn_abort(ptr); }
    int commit() { MDB_txn* t = ptr; ptr = nullptr; return mdb_txn_commit(t); }
};
struct Cursor
{
    MDB_cursor* ptr = nullptr;
    ~Cursor() { if (ptr) mdb_cursor_close(ptr); }
};
inline MDB_val val(std::string_view s) { return MDB_val{s.size(), const_cast<char*>(s.data())}; }
inline bool equal(MDB_val v, std::string_view s)
{ return v.mv_size == s.size() && (s.empty() || std::memcmp(v.mv_data, s.data(), s.size()) == 0); }
inline int get(MDB_txn* t, MDB_dbi d, std::string_view key, MDB_val& value)
{ MDB_val k = val(key); return mdb_get(t, d, &k, &value); }
inline int put(MDB_txn* t, MDB_dbi d, std::string_view key, std::string_view value)
{ MDB_val k = val(key), v = val(value); return mdb_put(t, d, &k, &v, 0); }
inline std::string encodeCount(std::uint64_t count)
{
    std::string result(8, '\0');
    for (std::size_t i=0; i<8; ++i) result[i] = static_cast<char>(count >> ((7-i)*8));
    return result;
}
inline std::uint64_t decodeCount(MDB_val v)
{
    const auto* p = static_cast<const unsigned char*>(v.mv_data);
    std::uint64_t result = 0;
    for (std::size_t i=0; i<8; ++i) result = (result << 8) | p[i];
    return result;
}
} // namespace detail

// Invalidate a completed staging marker in the same transaction as every
// evidence update. I is not authorization; a new full audit is still needed.
inline int invalidateEvidenceCompletion(MDB_txn* txn, MDB_dbi metadata)
{
    MDB_val state{};
    const int rc=detail::get(txn, metadata, "state", state);
    if (rc==MDB_NOTFOUND) return 0;
    if (rc) return rc;
    if (detail::equal(state, "I")) return 0;
    if (!detail::equal(state, "C")) return MDB_CORRUPTED;
    return detail::put(txn, metadata, "state", "I");
}

inline bool Bindings::putEvidence(const MDB_val& key, const MDB_val& value, const Evidence& proof)
{
    try { return putEvidenceBatch(std::vector<EvidenceInput>{{key,value,proof}}); }
    catch (...) { return false; }
}

inline bool Bindings::putEvidenceBatch(const std::vector<EvidenceInput>& records)
{
    if (!ready_.load(std::memory_order_acquire) || records.empty() || records.size()>maxBatchRecords) return false;
    try {
        std::lock_guard<std::mutex> evidenceLock(evidenceMutex_);
        std::lock_guard<std::mutex> writerLock(writerMutex_);
        detail::Transaction txn;
        if (mdb_txn_begin(dest_, nullptr, 0, &txn.ptr)) return false;
        for (const auto& record : records) {
            const auto& key=record.key; const auto& value=record.value; const auto& proof=record.proof;
            if (key.mv_size!=52 || !key.mv_data || (value.mv_size!=84 && value.mv_size!=88) || !value.mv_data ||
                !proof.independentlyVerified || proof.exchange.size()>16 || proof.code.size()>32) return false;
            std::string encoded;
            encoded.reserve(12 + value.mv_size + proof.exchange.size() + proof.code.size());
            encoded.push_back(1); encoded.push_back(static_cast<char>(value.mv_size));
            encoded.push_back(static_cast<char>(proof.exchange.size()));
            encoded.push_back(static_cast<char>(proof.code.size()));
            for (int i=7;i>=0;--i) encoded.push_back(static_cast<char>(proof.verifiedTime >> (i*8)));
            encoded.append(static_cast<const char*>(value.mv_data), value.mv_size);
            encoded.append(proof.exchange); encoded.append(proof.code);
            MDB_val k=key, v{encoded.size(), encoded.data()};
            if (mdb_put(txn.ptr, evidenceDb_, &k, &v, 0)!=0) return false;
        }
        return invalidateEvidenceCompletion(txn.ptr, metaDb_)==0 && txn.commit()==0;
    } catch (...) { return false; }
}

inline bool Bindings::revokeEvidence(const MDB_val& key)
{
    if (!ready_.load(std::memory_order_acquire) || key.mv_size != 52 || !key.mv_data) return false;
    std::lock_guard<std::mutex> evidenceLock(evidenceMutex_);
    std::lock_guard<std::mutex> writerLock(writerMutex_);
    detail::Transaction txn;
    if (mdb_txn_begin(dest_, nullptr, 0, &txn.ptr)) return false;
    MDB_val k=key;
    return mdb_del(txn.ptr, evidenceDb_, &k, nullptr)==0 &&
           invalidateEvidenceCompletion(txn.ptr, metaDb_)==0 && txn.commit()==0;
}

#ifdef WT_AD_MIGRATION_TEST_HOOKS
// Controlled test-only fault injection. Never compiled into normal builds.
void adjustSourceStatForTest(MDB_stat& stat);
void afterStagedPutForTest();
#endif

// The caller supplies exclusive access to both environments for the whole
// initialization; this function does not coordinate unrelated bindings, other
// clients, or other processes. It commits the named DBI creation and warms a
// reader on both environments before publishing the immutable binding. An error
// after DBI creation may leave empty named databases in the isolated staging
// environment; no records or completion marker are written here.
inline Result prepareBindings(MDB_env* source, MDB_env* dest, Bindings& binding)
{
    Result out;
    if (detail::inStageCallback) { out.code = ResultCode::ReentrantCall; return out; }
    if (!source || !dest) return out;
    if (source == dest) { out.code = ResultCode::SameEnvironment; return out; }
#ifdef _WIN32
    out.code = ResultCode::UnsupportedPlatform;
    return out;
#else
    std::lock_guard<std::mutex> lock(binding.writerMutex_);
    if (binding.ready_.load(std::memory_order_acquire)) return out;
    unsigned sf = 0, df = 0;
    int rc = mdb_env_get_flags(source, &sf);
    if (rc) { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
    if ((rc = mdb_env_get_flags(dest, &df)) != 0)
    { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
    if (!(sf & MDB_RDONLY) || (df & MDB_RDONLY) || (sf & MDB_NOLOCK) ||
        (df & (MDB_NOLOCK | MDB_NOSYNC | MDB_NOMETASYNC | MDB_MAPASYNC)))
    { out.code = ResultCode::UnsafeEnvironment; return out; }
    mdb_filehandle_t sfd, dfd;
    if ((rc = mdb_env_get_fd(source, &sfd)) != 0)
    { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
    if ((rc = mdb_env_get_fd(dest, &dfd)) != 0)
    { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
    struct stat ss{}, ds{};
    if (fstat(sfd, &ss) != 0 || fstat(dfd, &ds) != 0 || !S_ISREG(ss.st_mode) || !S_ISREG(ds.st_mode))
    { out.code = ResultCode::UnsafeEnvironment; return out; }
    if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino)
    { out.code = ResultCode::SameEnvironment; return out; }
    detail::Transaction st, dt, warm;
    MDB_dbi sourceDb = 0, stageDb = 0, metaDb = 0, evidenceDb = 0;
    if ((rc = mdb_txn_begin(source, nullptr, MDB_RDONLY, &st.ptr)) != 0 ||
        (rc = mdb_dbi_open(st.ptr, nullptr, 0, &sourceDb)) != 0 ||
        (rc = st.commit()) != 0)
    { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
    if ((rc = mdb_txn_begin(dest, nullptr, 0, &dt.ptr)) != 0 ||
        (rc = mdb_dbi_open(dt.ptr, stageName, MDB_CREATE, &stageDb)) != 0 ||
        (rc = mdb_dbi_open(dt.ptr, metadataName, MDB_CREATE, &metaDb)) != 0 ||
        (rc = mdb_dbi_open(dt.ptr, evidenceName, MDB_CREATE, &evidenceDb)) != 0)
    { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
    unsigned evidenceFlags = 0;
    if ((rc = mdb_dbi_flags(dt.ptr, evidenceDb, &evidenceFlags)) != 0)
    { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
    if (evidenceFlags != 0) { out.code = ResultCode::UnsupportedDatabaseFlags; return out; }
    if ((rc = dt.commit()) != 0 ||
        (rc = mdb_txn_begin(dest, nullptr, MDB_RDONLY, &warm.ptr)) != 0)
    { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
    binding.source_ = source;
    binding.dest_ = dest;
    binding.sourceDb_ = sourceDb;
    binding.stageDb_ = stageDb;
    binding.metaDb_ = metaDb;
    binding.evidenceDb_ = evidenceDb;
    binding.ready_.store(true, std::memory_order_release);
    out.code = ResultCode::Prepared;
    return out;
#endif
}

// Source is read-only; dest is a distinct private environment with maxdbs >= 2.
// Caller MUST stop/drain writers and gate old binaries; this function cannot
// establish either condition or verify independent evidence provenance. One
// invocation validates the whole previous prefix, then adds at most batchLimit
// records (<=4096) in one atomic destination transaction, together with its
// cursor/count/state manifest. A crash leaves the last committed batch intact.
// Values are copied unchanged; source endian and ABI are part of the manifest.
// A completed manifest is not a deployment/publish decision: staging must not
// be made visible before Complete and external verification. A source commit
// after our final check remains possible without the required external gate.
// The prefix scan uses constant application memory but time grows with the
// committed prefix. LMDB page/map/storage limits still apply. Collisions that
// already overwrote V1 bars cannot be detected or recovered by this operation.
// lookup may not call LMDB on either participating environment, close either
// environment/DBI, or initiate another migration and wait for it. Direct
// same-thread stageBatch reentry is rejected; asynchronous cross-thread
// reentry through external code is a caller responsibility. No target writer
// transaction or process-global mutex is held while lookup executes.
inline Result Bindings::stageBatchInternal(const Bindings& binding, LegacySourceHost host,
                                           LegacyValueLayout layout, const EvidenceLookup& lookup,
                                           std::size_t batchLimit)
{
    Result out;
    if (detail::inStageCallback) { out.code = ResultCode::ReentrantCall; return out; }
    if (!binding.ready_.load(std::memory_order_acquire))
    { out.code = ResultCode::UnpreparedBindings; return out; }
    MDB_env* source = binding.source_;
    MDB_env* dest = binding.dest_;
    if (!source || !dest || !lookup || batchLimit == 0 || batchLimit > maxBatchRecords ||
        binding.stageDb_ == binding.metaDb_ ||
        (host != LegacySourceHost::LittleEndian && host != LegacySourceHost::BigEndian) ||
        (layout != LegacyValueLayout::Aligned64 && layout != LegacyValueLayout::Packed64))
        return out;
    if (source == dest) { out.code = ResultCode::SameEnvironment; return out; }
#ifdef _WIN32
    out.code = ResultCode::UnsupportedPlatform;
    return out; // Backing-file identity is not yet verified on Windows.
#else
    try
    {
        unsigned sf = 0, df = 0;
        int rc = mdb_env_get_flags(source, &sf);
        if (rc) { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        rc = mdb_env_get_flags(dest, &df);
        if (rc) { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
        if (!(sf & MDB_RDONLY) || (df & MDB_RDONLY) || (sf & MDB_NOLOCK) ||
            (df & (MDB_NOLOCK | MDB_NOSYNC | MDB_NOMETASYNC | MDB_MAPASYNC)))
        { out.code = ResultCode::UnsafeEnvironment; return out; }
        mdb_filehandle_t sfd, dfd;
        if ((rc = mdb_env_get_fd(source, &sfd)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        if ((rc = mdb_env_get_fd(dest, &dfd)) != 0)
        { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
        struct stat ss{}, ds{};
        if (fstat(sfd, &ss) != 0 || fstat(dfd, &ds) != 0 || !S_ISREG(ss.st_mode) || !S_ISREG(ds.st_mode))
        { out.code = ResultCode::UnsafeEnvironment; return out; }
        if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino)
        { out.code = ResultCode::SameEnvironment; return out; }
        detail::Transaction st;
        if ((rc = mdb_txn_begin(source, nullptr, MDB_RDONLY, &st.ptr)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        const auto sourceTxnId = mdb_txn_id(st.ptr);
        const std::string marker = std::string("wt-ad-minute-stage-v2:") +
            std::to_string(static_cast<unsigned long long>(ss.st_dev)) + ":" +
            std::to_string(static_cast<unsigned long long>(ss.st_ino)) + ":" +
            std::to_string(static_cast<unsigned long long>(sourceTxnId)) + ":" +
            std::to_string(static_cast<int>(host)) + ":" + std::to_string(static_cast<int>(layout));
        const MDB_dbi sourceDb = binding.sourceDb_;
        unsigned dbFlags = 0;
        if ((rc = mdb_dbi_flags(st.ptr, sourceDb, &dbFlags)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        if (dbFlags != 0) { out.code = ResultCode::UnsupportedDatabaseFlags; return out; }
        const MDB_dbi stageDb = binding.stageDb_, metaDb = binding.metaDb_;
        std::uint64_t previousCount = 0;
        std::string previousKey;
        bool wasComplete = false;
        std::size_t targetTxnId = 0;
        MDB_stat stageStat{};
        {
            // Never hold this lock while calling lookup. It coordinates every
            // target read/write transaction made by this binding.
            std::lock_guard<std::mutex> targetLock(binding.writerMutex_);
            detail::Transaction dt;
            if ((rc = mdb_txn_begin(dest, nullptr, MDB_RDONLY, &dt.ptr)) != 0)
            { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
            for (MDB_dbi db : {stageDb, metaDb})
            {
                if ((rc = mdb_dbi_flags(dt.ptr, db, &dbFlags)) != 0)
                { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
                if (dbFlags != 0) { out.code = ResultCode::UnsupportedDatabaseFlags; return out; }
            }
            MDB_stat metaStat{};
            if ((rc = mdb_stat(dt.ptr, metaDb, &metaStat)) != 0 || (rc = mdb_stat(dt.ptr, stageDb, &stageStat)) != 0)
            { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
            MDB_val metadata{};
            rc = detail::get(dt.ptr, metaDb, "identity", metadata);
            if (rc == MDB_NOTFOUND)
            {
                if (metaStat.ms_entries != 0 || stageStat.ms_entries != 0)
                { out.code = ResultCode::UnexpectedStagingContents; return out; }
            }
            else if (rc != 0)
            { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
            else
            {
                if (metaStat.ms_entries != 4 || !detail::equal(metadata, marker))
                { out.code = ResultCode::MetadataMismatch; return out; }
                MDB_val count{}, last{}, state{};
                if (detail::get(dt.ptr, metaDb, "count", count) != 0 || count.mv_size != 8 ||
                    detail::get(dt.ptr, metaDb, "last", last) != 0 || (last.mv_size != 0 && last.mv_size != v1_size) ||
                    detail::get(dt.ptr, metaDb, "state", state) != 0 || state.mv_size != 1 ||
                    (!detail::equal(state, "I") && !detail::equal(state, "C")))
                { out.code = ResultCode::MetadataMismatch; return out; }
                previousCount = detail::decodeCount(count);
                if ((previousCount == 0) != (last.mv_size == 0) || previousCount != stageStat.ms_entries)
                { out.code = ResultCode::MetadataMismatch; return out; }
                if (last.mv_size) previousKey.assign(static_cast<const char*>(last.mv_data), last.mv_size);
                wasComplete = detail::equal(state, "C");
            }
            targetTxnId = mdb_txn_id(dt.ptr);
        }
        out.committedRecords = previousCount;
        detail::Cursor cur;
        if ((rc = mdb_cursor_open(st.ptr, sourceDb, &cur.ptr)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        MDB_val key{}, value{};
        rc = mdb_cursor_get(cur.ptr, &key, &value, MDB_FIRST);
        std::uint64_t visited = 0;
        std::size_t added = 0;
        std::string lastKey;
        struct Candidate { Key key; std::array<std::uint8_t, 88> value{}; std::size_t size = 0; };
        std::vector<Candidate> pending, prefix;
        pending.reserve(batchLimit);
        prefix.reserve(batchLimit);
        auto verifyPrefix = [&]() -> bool {
            if (prefix.empty()) return true;
            std::lock_guard<std::mutex> targetLock(binding.writerMutex_);
            detail::Transaction read;
            int check = mdb_txn_begin(dest, nullptr, MDB_RDONLY, &read.ptr);
            if (check != 0) { out.code = ResultCode::DestinationError; out.lmdbError = check; return false; }
            if (mdb_txn_id(read.ptr) != targetTxnId)
            { out.code = ResultCode::DestinationChanged; return false; }
            for (auto& item : prefix)
            {
                MDB_val k{item.key.size(), item.key.data()}, written{};
                check = mdb_get(read.ptr, stageDb, &k, &written);
                if (check == MDB_NOTFOUND || (check == 0 && (written.mv_size != item.size ||
                    std::memcmp(written.mv_data, item.value.data(), item.size) != 0)))
                { out.code = ResultCode::Conflict; return false; }
                if (check != 0) { out.code = ResultCode::DestinationError; out.lmdbError = check; return false; }
            }
            prefix.clear();
            return true;
        };
        while (rc == 0 && (visited < previousCount || (!wasComplete && added < batchLimit)))
        {
            Evidence proof;
            bool provided = false;
            {
                detail::ReentryGuard guard;
                provided = lookup(key, value, proof);
            }
            if (!provided || !proof.independentlyVerified)
            { out.code = ResultCode::MissingEvidence; return out; }
            LegacyEvidence legacy{host, layout, TimeEvidence::IndependentlyVerified, proof.verifiedTime};
            Key converted{};
            out.codecStatus = convertLegacyCandidate(key.mv_data, key.mv_size, value.mv_data,
                value.mv_size, proof.exchange, proof.code, legacy, converted);
            if (out.codecStatus != Status::Ok) { out.code = ResultCode::InvalidRecord; return out; }
            const bool checkingPrefix = visited < previousCount;
            Candidate candidate;
            candidate.key = converted;
            candidate.size = value.mv_size; // codec checked fixed 84/88-byte layout
            std::memcpy(candidate.value.data(), value.mv_data, value.mv_size);
            if (!checkingPrefix)
            {
                if (!verifyPrefix()) return out;
                pending.push_back(candidate);
                ++added;
            }
            else
            {
                prefix.push_back(candidate);
                if (prefix.size() == batchLimit && !verifyPrefix()) return out;
            }
            lastKey.assign(static_cast<const char*>(key.mv_data), key.mv_size);
            ++visited;
            if (visited == previousCount && lastKey != previousKey)
            { out.code = ResultCode::MetadataMismatch; return out; }
            rc = mdb_cursor_get(cur.ptr, &key, &value, MDB_NEXT);
        }
        const bool cursorExhausted = rc == MDB_NOTFOUND;
        if (!verifyPrefix()) return out;
        if (rc != 0 && !cursorExhausted)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        if (visited < previousCount || (wasComplete && !cursorExhausted))
        { out.code = ResultCode::MetadataMismatch; return out; }
        // Count comes from the same read snapshot as the cursor.
        MDB_stat sourceStat{};
        if ((rc = mdb_stat(st.ptr, sourceDb, &sourceStat)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
#ifdef WT_AD_MIGRATION_TEST_HOOKS
        adjustSourceStatForTest(sourceStat);
#endif
        if ((cursorExhausted && visited != sourceStat.ms_entries) ||
            (!cursorExhausted && visited >= sourceStat.ms_entries))
        { out.code = ResultCode::SourceInconsistent; return out; }
        const bool finished = cursorExhausted && visited == sourceStat.ms_entries;
        if (wasComplete && !finished) { out.code = ResultCode::MetadataMismatch; return out; }

        // Reject any target mutation since the initial metadata snapshot.
        // This caller-owned mutex is never held across evidence callbacks.
        std::lock_guard<std::mutex> writerLock(binding.writerMutex_);
        detail::Transaction wt; // aborted before writerLock is released
        if ((rc = mdb_txn_begin(dest, nullptr, 0, &wt.ptr)) != 0)
        { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
        if (targetTxnId == (std::numeric_limits<std::size_t>::max)() ||
            mdb_txn_id(wt.ptr) != targetTxnId + 1)
        { out.code = ResultCode::DestinationChanged; return out; }
        MDB_envinfo info{};
        if ((rc = mdb_env_info(source, &info)) != 0)
        { out.code = ResultCode::SourceError; out.lmdbError = rc; return out; }
        if (info.me_last_txnid != sourceTxnId)
        { out.code = ResultCode::SourceChanged; return out; }
        if (wasComplete)
        {
            out.committedRecords = visited;
            out.code = ResultCode::Complete;
            return out; // writer transaction aborts without modifying metadata
        }
        for (auto& item : pending)
        {
            MDB_val v2key{item.key.size(), item.key.data()}, v{item.size, item.value.data()}, written{};
            int put = mdb_put(wt.ptr, stageDb, &v2key, &v, MDB_NOOVERWRITE);
            if (put == MDB_KEYEXIST) { out.code = ResultCode::Conflict; return out; }
            if (put != 0) { out.code = ResultCode::DestinationError; out.lmdbError = put; return out; }
#ifdef WT_AD_MIGRATION_TEST_HOOKS
            afterStagedPutForTest();
#endif
            int read = mdb_get(wt.ptr, stageDb, &v2key, &written);
            if (read != 0 || written.mv_size != item.size ||
                std::memcmp(written.mv_data, item.value.data(), item.size) != 0)
            { out.code = ResultCode::Conflict; out.lmdbError = read; return out; }
        }
        if ((rc = mdb_stat(wt.ptr, stageDb, &stageStat)) != 0)
        { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
        if (stageStat.ms_entries != visited)
        { out.code = ResultCode::UnexpectedStagingContents; return out; }
        const std::string count = detail::encodeCount(visited);
        if ((rc = detail::put(wt.ptr, metaDb, "identity", marker)) != 0 ||
            (rc = detail::put(wt.ptr, metaDb, "count", count)) != 0 ||
            (rc = detail::put(wt.ptr, metaDb, "last", lastKey)) != 0 ||
            (rc = detail::put(wt.ptr, metaDb, "state", finished ? "C" : "I")) != 0 ||
            (rc = wt.commit()) != 0)
        { out.code = ResultCode::DestinationError; out.lmdbError = rc; return out; }
        out.committedRecords = visited;
        out.code = finished ? ResultCode::Complete : ResultCode::Incomplete;
        return out;
    }
    catch (...)
    {
        out.code = ResultCode::CallbackError;
        return out;
    }
#endif
}

// A general callback cannot supply an immutable or revocable snapshot. Never
// call it or write a staging marker through this legacy overload.
inline Result stageBatch(const Bindings&, LegacySourceHost, LegacyValueLayout,
                         const EvidenceLookup&, std::size_t)
{
    Result out;
    out.code = ResultCode::EvidenceCapabilityRequired;
    return out;
}

// Evidence updates and the destination commit share one mutex. The commit is
// linearized before a concurrent revocation that acquires this mutex later.
// A C marker describes that commit only, not a publish gate. Controlled evidence
// updates atomically demote C to I. Every call audits the entire committed prefix.
inline Result stageBatch(const Bindings& binding, LegacySourceHost host,
                         LegacyValueLayout layout, std::size_t batchLimit)
{
    std::lock_guard<std::mutex> lock(binding.evidenceMutex_);
    const EvidenceLookup lookup = [&binding](const MDB_val& key, const MDB_val& value, Evidence& proof) {
        detail::Transaction txn;
        if (mdb_txn_begin(binding.dest_, nullptr, MDB_RDONLY, &txn.ptr) != 0) return false;
        MDB_val k=key, encoded{};
        if (mdb_get(txn.ptr, binding.evidenceDb_, &k, &encoded) != 0 || encoded.mv_size < 12) return false;
        const auto* p = static_cast<const unsigned char*>(encoded.mv_data);
        const std::size_t valueSize=p[1], exchangeSize=p[2], codeSize=p[3];
        if (p[0]!=1 || (valueSize!=84 && valueSize!=88) || exchangeSize>16 || codeSize>32 ||
            encoded.mv_size!=12+valueSize+exchangeSize+codeSize || value.mv_size!=valueSize ||
            std::memcmp(p+12, value.mv_data, valueSize)!=0) return false;
        proof.verifiedTime=0;
        for (unsigned i=0;i<8;++i) proof.verifiedTime=(proof.verifiedTime<<8)|p[4+i];
        proof.exchange.assign(reinterpret_cast<const char*>(p+12+valueSize), exchangeSize);
        proof.code.assign(reinterpret_cast<const char*>(p+12+valueSize+exchangeSize), codeSize);
        proof.independentlyVerified=true;
        return true;
    };
    return Bindings::stageBatchInternal(binding, host, layout, lookup, batchLimit);
}
} // namespace wt_ad_bar_migration
