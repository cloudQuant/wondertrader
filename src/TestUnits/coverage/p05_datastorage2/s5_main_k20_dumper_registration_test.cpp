// 迭代03 第五会话主线程：决策 K20=B（2026-11-04 用户裁定，BUG-P10-006）
// IDataWriter 生命周期 V1 表追加版本化 add/remove dumper（minor 1）：登记与写入路径同步（写入路径对快照调用 dumper，
// 不持注册锁），重复 ID 拒绝并保留原实例，dumper 回调内重入登记不死锁，request_stop 之后拒绝登记。
// 宿主共享所有权至 DRAINED 由 DataManager/WtDtRunner 承担（见证据 README）。
#include "r4_ad_fixture.h"

extern "C" const WtDataWriterLifecycleApiV1* getDataWriterLifecycleApiV1(uint32_t requestedMajor);
extern "C" IDataWriter* createWriter();
extern "C" void deleteWriter(IDataWriter*& writer);

namespace s5k20 {
using namespace r4fx;
namespace {

const uint32_t kD1 = 20240103;

class CountingDumper : public IHisDataDumper
{
public:
	std::function<void()> onTick;
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override { return true; }
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override
	{
		_ticks++;
		if (onTick)
			onTick();
		return true;
	}
	uint32_t ticks() const { return _ticks; }

private:
	uint32_t _ticks = 0;
};

} // namespace

class S5MainK20DumperRegistration : public R4WriterADTest
{
protected:
	const WtDataWriterLifecycleApiV1* api = nullptr;
	WTSContractInfo* ct = nullptr;
	uint32_t seq = 0;

	void prepare()
	{
		api = getDataWriterLifecycleApiV1(WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR);
		ASSERT_NE(api, nullptr);
		WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, false, false, false, false);
		init_writer(cfg);
		WTSCommodityInfo* comm = _bd->add_commodity("EXW1", "rb", ContractCategory::CC_Future);
		ct = _bd->add_contract("EXW1", "rb2301", comm);
	}

	void tick()
	{
		const uint32_t t = 93000000 + (++seq) * 1000;
		WTSTickStruct ts = make_tick("EXW1", "rb2301", kD1, kD1, t, 3900.0 + seq, 5.0, 500.0, 100.0, 0.0, 10.0 + seq, 500.0);
		write_or_fail(tick_of(ct, ts), 0);
	}

	// The fixture writer is constructed directly (not through createWriter), so the
	// versioned table, which accepts only module-created writers, is exercised separately.
	WtDataWriterLifecycleV1Status add(const char* id, IHisDataDumper* d) { return writer()->addDumperV1(id, d); }
	WtDataWriterLifecycleV1Status remove(const char* id) { return writer()->removeDumperV1(id); }
};

TEST_F(S5MainK20DumperRegistration, TableOffersVersionedDumperRegistration)
{
	const WtDataWriterLifecycleApiV1* table = getDataWriterLifecycleApiV1(WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR);
	ASSERT_NE(table, nullptr);
	EXPECT_EQ(table->struct_size, (uint32_t)sizeof(WtDataWriterLifecycleApiV1));
	EXPECT_GT(table->struct_size, (uint32_t)WT_DATA_WRITER_LIFECYCLE_V1_MINOR0_SIZE);
	EXPECT_EQ(table->abi_version & 0xFFFFu, 1u);
	EXPECT_NE(0u, table->capabilities & WT_DATA_WRITER_LIFECYCLE_V1_CAP_DUMPERS);
	EXPECT_NE(nullptr, table->add_dumper);
	EXPECT_NE(nullptr, table->remove_dumper);

	// Table entries accept only writers created by this module and route to the registry.
	IDataWriter* created = createWriter();
	ASSERT_NE(created, nullptr);
	CountingDumper a, b;
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, table->add_dumper(static_cast<void*>(created), "a", &a));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_DUPLICATE, table->add_dumper(static_cast<void*>(created), "a", &b));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, table->remove_dumper(static_cast<void*>(created), "a"));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, table->remove_dumper(static_cast<void*>(created), "a"));
	int foreign = 0;
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, table->add_dumper(static_cast<void*>(&foreign), "x", &a));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, table->request_stop(static_cast<void*>(created)));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_ERROR, table->add_dumper(static_cast<void*>(created), "late", &a));
	created->release();
	deleteWriter(created);
}

TEST_F(S5MainK20DumperRegistration, DuplicateIdIsRejectedAndTheOriginalKeepsReceiving)
{
	prepare();
	CountingDumper original;
	CountingDumper replacement;
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, add("rec", &original));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_DUPLICATE, add("rec", &replacement));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, add("", &replacement));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, add("null", NULL));
	tick();
	EXPECT_EQ(1u, original.ticks());
	EXPECT_EQ(0u, replacement.ticks());

	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, remove("rec"));
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, remove("rec"));
	tick();
	EXPECT_EQ(1u, original.ticks()) << "a removed dumper is not called by later writes";
}

TEST_F(S5MainK20DumperRegistration, RegisteringFromInsideADumperCallbackDoesNotDeadlock)
{
	prepare();
	CountingDumper first;
	CountingDumper late;
	bool registered = false;
	first.onTick = [&]() {
		if (!registered)
		{
			registered = true;
			EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, add("late", &late));
		}
	};
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, add("first", &first));
	tick();	// first registers "late" from inside its callback (snapshot iteration)
	EXPECT_TRUE(registered);
	EXPECT_EQ(1u, first.ticks());
	EXPECT_EQ(0u, late.ticks()) << "the running write uses the snapshot taken before the registration";
	tick();
	EXPECT_EQ(2u, first.ticks());
	EXPECT_EQ(1u, late.ticks());
}

TEST_F(S5MainK20DumperRegistration, RegistrationAfterStopRequestIsRejected)
{
	prepare();
	CountingDumper d;
	writer()->requestStop();
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_ERROR, add("after_stop", &d));
}

} // namespace s5k20
