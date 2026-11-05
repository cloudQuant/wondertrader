// Built as an isolated test TU. The included test support has private-access
// exposure and fixture types, but only the six cases below are executed.
#include "../../coverage/p10_storage_writer/p10_rt_block_files_test.cpp"

namespace {
struct InsertGate {
    std::mutex mutex;
    std::condition_variable changed;
    WtDataWriter* writer = nullptr;
    unsigned target = 0;
    bool inserted = false, allowInsert = false, contended = false;
    bool snapshot = false, allowSnapshot = false, callback = false, allowCallback = false;
    long owners = 0;
    std::weak_ptr<void> lifetime;
    std::thread::id insertThread, callbackThread;
    template<class P> bool wait(P p) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), p);
    }
    void openAll() {
        std::lock_guard<std::mutex> lock(mutex);
        allowInsert = allowSnapshot = allowCallback = true;
        changed.notify_all();
    }
};
InsertGate* activeGate = nullptr; // One test process, registered before worker launch.

class InsertSink : public IDataWriterSink {
public:
    explicit InsertSink(InsertGate& g) : gate(g) {}
    IBaseDataMgr* getBDMgr() override { return nullptr; }
    bool canSessionReceive(const char*) override { return true; }
    void finish() {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.callbackThread = std::this_thread::get_id();
        gate.callback = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.allowCallback; });
    }
    void broadcastTick(WTSTickData*) override { finish(); }
    void broadcastOrdQue(WTSOrdQueData*) override { finish(); }
    void broadcastOrdDtl(WTSOrdDtlData*) override { finish(); }
    void broadcastTrans(WTSTransData*) override { finish(); }
    CodeSet* getSessionComms(const char*) override { return &comms; }
    uint32_t getTradingDate(const char*) override { return kTickDate; }
    void outputLog(WTSLogLevel, const char*) override {}
    InsertGate& gate;
    CodeSet comms;
};
void onContention(void* ptr) noexcept {
    auto& gate = *static_cast<InsertGate*>(ptr);
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.contended = true;
    gate.changed.notify_all();
}
}

extern "C" void p10_fixture_insert(WtDataWriter* writer, unsigned kind) {
    InsertGate* gate = activeGate;
    if (!gate || gate->writer != writer || gate->target != kind) return;
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->insertThread = std::this_thread::get_id();
    gate->inserted = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&] { return gate->allowInsert; });
}
extern "C" void p10_fixture_snapshot(WtDataWriter* writer, unsigned kind,
                                     long owners, std::weak_ptr<void> lifetime) {
    InsertGate* gate = activeGate;
    if (!gate || gate->writer != writer || gate->target != kind) return;
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->owners = owners;
    gate->lifetime = std::move(lifetime);
    gate->snapshot = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&] { return gate->allowSnapshot; });
}

namespace {
class FirstInsertFixture : public P10RtBlockTest {
protected:
    void exercise(unsigned kind) {
        const size_t bytes = sizeof(RTTickCache) + sizeof(TickCacheItem) * 32;
        std::vector<uint64_t> cache((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
        InsertGate gate;
        InsertSink sink(gate);
        WtDataWriter writer;
        writer._base_dir = _base;
        writer._save_tick_log = false;
        writer._async_proc = true;
        writer._log_group_size = 1000;
        writer.IDataWriter::init(nullptr, &sink);
        writer._tick_cache_block = reinterpret_cast<RTTickCache*>(cache.data());
        writer._tick_cache_block->_capacity = 32;
        writer._tick_cache_block->_size = 0;
        {
            StdUniqueLock lock(writer._accept_mtx);
            writer._lifecycle_state = WtDataWriter::LifecycleState::Ready;
            writer._accepting = true;
        }
        gate.writer = &writer;
        gate.target = kind;
        activeGate = &gate;
        p10_writer_test_hooks::Hooks hooks;
        hooks.context = &gate;
        hooks.snapshot_map_lock_contended = onContention;
        hooks.skip_initial_check_wait = true;
        ASSERT_TRUE(p10_writer_test_hooks::install(&writer, hooks));

        bool accepted = false;
        if (kind == 1 || kind == 5 || kind == 6) {
            auto tickData = make_tick(7);
            auto* tick = WTSTickData::create(tickData);
            tick->setContractInfo(_ct);
            accepted = writer.writeTick(tick, 0);
            tick->release();
        } else if (kind == 2) {
            WTSTransStruct raw{};
            std::snprintf(raw.exchg, sizeof(raw.exchg), "%s", _ct->getExchg());
            std::snprintf(raw.code, sizeof(raw.code), "%s", _ct->getCode());
            raw.trading_date = raw.action_date = kTickDate;
            raw.action_time = 93002200; raw.index = 5002; raw.price = 3512.5; raw.volume = 13;
            auto* item = WTSTransData::create(raw); item->setContractInfo(_ct);
            accepted = writer.writeTransaction(item); item->release();
        } else if (kind == 3) {
            WTSOrdDtlStruct raw{};
            std::snprintf(raw.exchg, sizeof(raw.exchg), "%s", _ct->getExchg());
            std::snprintf(raw.code, sizeof(raw.code), "%s", _ct->getCode());
            raw.trading_date = raw.action_date = kTickDate;
            raw.action_time = 93001100; raw.index = 4001; raw.price = 3511.5; raw.volume = 12;
            auto* item = WTSOrdDtlData::create(raw); item->setContractInfo(_ct);
            accepted = writer.writeOrderDetail(item); item->release();
        } else {
            WTSOrdQueStruct raw{};
            std::snprintf(raw.exchg, sizeof(raw.exchg), "%s", _ct->getExchg());
            std::snprintf(raw.code, sizeof(raw.code), "%s", _ct->getCode());
            raw.trading_date = raw.action_date = kTickDate;
            raw.action_time = 93003300; raw.price = 3513.5; raw.order_items = 2;
            auto* item = WTSOrdQueData::create(raw); item->setContractInfo(_ct);
            accepted = writer.writeOrderQueue(item); item->release();
        }
        const bool inserted = accepted && gate.wait([&] { return gate.inserted; });
        bool spawned = false;
        if (inserted) {
            writer._proc_chk.reset(new StdThread([&writer] { writer.check_loop(); }));
            spawned = true;
        }
        const bool contention = spawned && gate.wait([&] { return gate.contended; });
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.allowInsert = true;
            gate.changed.notify_all();
        }
        const bool snapshot = contention && gate.wait([&] { return gate.snapshot; });
        long owners = 0;
        std::weak_ptr<void> lifetime;
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            owners = gate.owners;
            lifetime = gate.lifetime;
            gate.allowSnapshot = true;
            gate.changed.notify_all();
        }
        const bool callback = snapshot && gate.wait([&] { return gate.callback; });
        std::thread::id worker, insertedThread, callbackThread, checker;
        { StdUniqueLock lock(writer._task_mtx); worker = writer._task_worker_id; }
        { StdUniqueLock lock(writer._check_mtx); checker = writer._check_worker_id; }
        { std::lock_guard<std::mutex> lock(gate.mutex); insertedThread = gate.insertThread; callbackThread = gate.callbackThread; }
        const bool aliveWhileSnapshotHeld = snapshot && owners >= 2 && !lifetime.expired();
        gate.openAll();
        writer.release();
        p10_writer_test_hooks::uninstall(&writer);
        activeGate = nullptr;

        const char* dirs[] = { "", "ticks", "trans", "orders", "queue", "min1", "min5" };
        const std::string path = _base + "rt/" + dirs[kind] + "/SHFE/rb2210.dmb";
        EXPECT_TRUE(accepted); EXPECT_TRUE(inserted); EXPECT_TRUE(spawned);
        EXPECT_TRUE(contention); EXPECT_TRUE(snapshot); EXPECT_TRUE(callback);
        EXPECT_TRUE(aliveWhileSnapshotHeld); EXPECT_TRUE(lifetime.expired());
        EXPECT_NE(worker, std::thread::id()); EXPECT_EQ(worker, insertedThread);
        EXPECT_EQ(worker, callbackThread); EXPECT_NE(worker, checker);
        const std::string disk = read_file_bytes(path);
        ASSERT_GE(disk.size(), sizeof(RTDayBlockHeader));
        RTDayBlockHeader header{};
        std::memcpy(&header, disk.data(), sizeof(header));
        EXPECT_EQ(header._size, 1u);
        if (kind == 1) {
            ASSERT_GE(disk.size(), sizeof(RTTickBlock) + sizeof(WTSTickStruct));
            WTSTickStruct value{};
            std::memcpy(&value, disk.data()+sizeof(RTTickBlock), sizeof(value));
            EXPECT_EQ(value.trading_date, kTickDate); EXPECT_DOUBLE_EQ(value.price, 3607.0);
        } else if (kind == 2) {
            ASSERT_GE(disk.size(), sizeof(RTTransBlock) + sizeof(WTSTransStruct));
            WTSTransStruct value{};
            std::memcpy(&value, disk.data()+sizeof(RTTransBlock), sizeof(value));
            EXPECT_EQ(value.index, 5002u); EXPECT_DOUBLE_EQ(value.price, 3512.5);
        } else if (kind == 3) {
            ASSERT_GE(disk.size(), sizeof(RTOrdDtlBlock) + sizeof(WTSOrdDtlStruct));
            WTSOrdDtlStruct value{};
            std::memcpy(&value, disk.data()+sizeof(RTOrdDtlBlock), sizeof(value));
            EXPECT_EQ(value.index, 4001u); EXPECT_DOUBLE_EQ(value.price, 3511.5);
        } else if (kind == 4) {
            ASSERT_GE(disk.size(), sizeof(RTOrdQueBlock) + sizeof(WTSOrdQueStruct));
            WTSOrdQueStruct value{};
            std::memcpy(&value, disk.data()+sizeof(RTOrdQueBlock), sizeof(value));
            EXPECT_EQ(value.order_items, 2u); EXPECT_DOUBLE_EQ(value.price, 3513.5);
        } else {
            ASSERT_GE(disk.size(), sizeof(RTKlineBlock) + sizeof(WTSBarStruct));
            WTSBarStruct value{};
            std::memcpy(&value, disk.data()+sizeof(RTKlineBlock), sizeof(value));
            EXPECT_EQ(value.date, kTickDate); EXPECT_DOUBLE_EQ(value.close, 3607.0);
        }
    }
};
TEST_F(FirstInsertFixture, Tick) { exercise(1); }
TEST_F(FirstInsertFixture, Transaction) { exercise(2); }
TEST_F(FirstInsertFixture, OrderDetail) { exercise(3); }
TEST_F(FirstInsertFixture, OrderQueue) { exercise(4); }
TEST_F(FirstInsertFixture, Minute1) { exercise(5); }
TEST_F(FirstInsertFixture, Minute5) { exercise(6); }
}
