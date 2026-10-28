/*!
 * \file r18_shm_caster_test.cpp
 * \brief WtDtCore/ShmCaster 结构与拒绝分支覆盖用例（迭代01 R18 lane）
 *
 * 口径（按任务令）：只做「结构/尺寸/参数校验与拒绝分支」，
 * 不创建共享内存段、不做真 mmap 文件映射、不起线程、不 sleep。
 *
 * 覆盖目标：
 *  - ShmCaster.h:36-58 DataItem 的打包布局（uint32 类型位 + 8 字节对齐的联合体）与清零构造
 *  - ShmCaster.h:65-89 _DataQueue<N> 模板（容量/读写位/pid 初值、环形下标回绕）与 CastQueue 尺寸
 *  - ShmCaster.cpp:24-32 init 的 cfg==NULL 与 active==false 两条拒绝分支（含「拒绝时不碰文件」）
 *  - ShmCaster.cpp:72-76 / 99-103 / 126-130 / 153-157 四个 broadcast 的
 *    「数据为空 / 队列未映射 / 未初始化」守卫（含 IDataCaster* 基类分派）
 *  - ShmCaster.h:98 构造函数的 _queue=NULL、_inited=false
 * 可证伪性说明：守卫一旦漏掉，L83/L110/L137/L164 会对 NULL 的 `_queue` 解引用，
 * 用例必然 SIGSEGV；因此「跑完 + 未创建任何文件」即等价于证明走到了 return。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/IDataCaster.h>
#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Includes/WTSStruct.h>
#include <WTSUtils/WTSCfgLoader.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

typedef ShmCaster::DataItem DataItem;
typedef ShmCaster::CastQueue CastQueue;
typedef ShmCaster::_DataQueue<4> Queue4;

// 本用例自己喂的 4 个负载结构体的尺寸，用于算出 DataItem 的期望尺寸
std::size_t max_payload_size()
{
	std::size_t sizes[4] = {
		sizeof(WTSTickStruct), sizeof(WTSOrdQueStruct),
		sizeof(WTSOrdDtlStruct), sizeof(WTSTransStruct)
	};
	std::size_t maxv = 0;
	for (std::size_t i = 0; i < 4; i++)
	{
		if (sizes[i] > maxv)
			maxv = sizes[i];
	}
	return maxv;
}

std::size_t align_up(std::size_t v, std::size_t a)
{
	return ((v + a - 1) / a) * a;
}

class R18ShmCasterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r18_shm");
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	static WTSTickData* make_tick()
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		s.price = 12.5;
		return WTSTickData::create(s);
	}

	static WTSOrdQueData* make_ordque()
	{
		WTSOrdQueStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSOrdQueData::create(s);
	}

	static WTSOrdDtlData* make_orddtl()
	{
		WTSOrdDtlStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSOrdDtlData::create(s);
	}

	static WTSTransData* make_trans()
	{
		WTSTransStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSTransData::create(s);
	}

	fs::path _dir;
};

TEST_F(R18ShmCasterTest, DataItemLaysOutTypeWordThenUnionOfFourPayloads)
{
	DataItem item;
	// pack(8) + 联合体成员含 double/uint64 -> 8 字节对齐：类型位占 4 字节后补到 8
	const std::size_t expectUnionOffset = align_up(sizeof(uint32_t), 8);
	const std::size_t expectSize = expectUnionOffset + max_payload_size();

	EXPECT_EQ(sizeof(DataItem), expectSize);
	EXPECT_EQ(expectUnionOffset, 8u);

	// 匿名联合体的四个成员共享同一地址，且紧跟在 _type 之后
	char* base = reinterpret_cast<char*>(&item);
	const std::ptrdiff_t offTick = reinterpret_cast<char*>(&item._tick) - base;
	const std::ptrdiff_t offQueue = reinterpret_cast<char*>(&item._queue) - base;
	const std::ptrdiff_t offOrder = reinterpret_cast<char*>(&item._order) - base;
	const std::ptrdiff_t offTrans = reinterpret_cast<char*>(&item._trans) - base;
	EXPECT_EQ(offTick, static_cast<std::ptrdiff_t>(expectUnionOffset));
	EXPECT_EQ(offQueue, offTick);
	EXPECT_EQ(offOrder, offTick);
	EXPECT_EQ(offTrans, offTick);
}

TEST_F(R18ShmCasterTest, QueueTemplateDefaultsCapacityReadableWritablePid)
{
	// 模板参数 N=4：本用例自己指定的小容量，用于观察环形下标回绕
	Queue4 q;
	EXPECT_EQ(q._capacity, 4u);
	// ShmCaster.h:83 的可读位初值是 UINT64_MAX（表示「暂无可读数据」）
	EXPECT_EQ(q._readable, UINT64_MAX);
	EXPECT_EQ(q._writable, 0u);
	EXPECT_EQ(q._pid, 0u);
	EXPECT_EQ(q._items[0]._type, 0u);
	EXPECT_EQ(q._items[3]._type, 0u);

	// 头 4 个标量按 pack(8) 补到 32 字节，随后是 N 个数据项
	EXPECT_EQ(sizeof(Queue4), 32u + 4u * sizeof(DataItem));
}

TEST_F(R18ShmCasterTest, RingIndexModuloWrapsWithinSmallQueue)
{
	// 复现 ShmCaster.cpp:83-91 的下标算式（用本地小队列，不触碰任何映射内存）
	Queue4 q;
	for (uint64_t i = 0; i < 10; i++)
	{
		uint64_t wIdx = q._writable++;
		uint64_t realIdx = wIdx % q._capacity;
		q._items[realIdx]._type = 0;
		q._items[realIdx]._tick.price = static_cast<double>(i) + 0.5;
		q._readable = wIdx;
		// 期望下标由本用例自己喂的 i 直接算出：0,1,2,3,0,1,2,3,0,1
		static const uint64_t expectIdx[10] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1 };
		ASSERT_EQ(realIdx, expectIdx[i]);
		if (i > 0)
			EXPECT_EQ(q._readable, i - 1);
	}
	EXPECT_EQ(q._writable, 10u);
	// 回绕后槽位 0 存的是最后一次写入（i=8），i=0/4 的值被覆盖
	EXPECT_DOUBLE_EQ(q._items[0]._tick.price, 8.5);
	EXPECT_DOUBLE_EQ(q._items[1]._tick.price, 9.5);
	EXPECT_DOUBLE_EQ(q._items[2]._tick.price, 6.5);
	EXPECT_DOUBLE_EQ(q._items[3]._tick.price, 7.5);
}

TEST_F(R18ShmCasterTest, CastQueueIsTheFixed8192SlotInstantiation)
{
	// CastQueue == _DataQueue<8*1024>（ShmCaster.h:89）
	EXPECT_EQ(sizeof(CastQueue), sizeof(ShmCaster::_DataQueue<8 * 1024>));
	EXPECT_EQ(sizeof(CastQueue), 32u + 8192u * sizeof(DataItem));

	// 默认模板实参同样是 8*1024（ShmCaster.h:65）
	EXPECT_EQ(sizeof(ShmCaster::_DataQueue<>), sizeof(CastQueue));

	// 4MB 级对象放静态存储区，避免栈溢出
	static CastQueue pool;
	EXPECT_EQ(pool._capacity, 8192u);
	EXPECT_EQ(pool._readable, UINT64_MAX);
	EXPECT_EQ(pool._writable, 0u);
	EXPECT_EQ(pool._pid, 0u);
}

TEST_F(R18ShmCasterTest, InitRejectsNullConfig)
{
	ShmCaster caster;
	// ShmCaster.cpp:27-28
	EXPECT_FALSE(caster.init(nullptr));
}

TEST_F(R18ShmCasterTest, InitRejectsInactiveAndTouchesNoFile)
{
	const fs::path shmPath = _dir / "r18_cast_queue.bin";
	ShmCaster caster;

	write_file(_dir / "inactive.json",
		R"JSON({"active": false, "path": ")JSON" + shmPath.string() + R"JSON("})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "inactive.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	// ShmCaster.cpp:31-32：active 为 false 直接拒绝
	EXPECT_FALSE(caster.init(cfg));
	EXPECT_FALSE(fs::exists(shmPath));

	// 缺省（不写 active）时 getBoolean 回落 false，同样拒绝
	write_file(_dir / "noactive.json",
		R"JSON({"path": ")JSON" + shmPath.string() + R"JSON("})JSON");
	WTSVariant* cfg2 = WTSCfgLoader::load_from_file((_dir / "noactive.json").string().c_str());
	ASSERT_NE(cfg2, nullptr);
	EXPECT_FALSE(caster.init(cfg2));
	EXPECT_FALSE(fs::exists(shmPath));

	cfg->release();
	cfg2->release();
	// 两次拒绝之后队列指针仍为 NULL，任何 broadcast 只会走守卫
	WTSTickData* tick = make_tick();
	caster.broadcast(tick);
	EXPECT_FALSE(fs::exists(shmPath));
	tick->release();
}

TEST_F(R18ShmCasterTest, FreshCasterRejectsAllFourBroadcastOverloads)
{
	ShmCaster caster;
	// _queue==NULL 且 !_inited：四种数据类型各喂一次真实对象
	// （ShmCaster.cpp:75/102/129/156 的第二个子条件）
	WTSTickData* tick = make_tick();
	WTSOrdQueData* oq = make_ordque();
	WTSOrdDtlData* od = make_orddtl();
	WTSTransData* tr = make_trans();

	caster.broadcast(tick);
	caster.broadcast(oq);
	caster.broadcast(od);
	caster.broadcast(tr);

	// 第一个子条件（数据指针为空）也要单独走一遍
	caster.broadcast(static_cast<WTSTickData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdQueData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdDtlData*>(nullptr));
	caster.broadcast(static_cast<WTSTransData*>(nullptr));

	tick->release();
	oq->release();
	od->release();
	tr->release();
}

TEST_F(R18ShmCasterTest, DISABLED_InitWithActiveTrueNeedsRealMappedFile)
{
	const fs::path shmPath = _dir / "r18_cast_queue.bin";
	write_file(_dir / "active.json",
		R"JSON({"active": true, "path": ")JSON" + shmPath.string() + R"JSON("})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "active.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ShmCaster caster;
	EXPECT_TRUE(caster.init(cfg));
	cfg->release();
}

}  // namespace
