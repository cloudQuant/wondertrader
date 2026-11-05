/**
 * 迭代03 第五会话主线程：决策 K31=A（插件侧取得 DateMappingV1：插件可选导出绑定函数、宿主注入 V1 表）
 * BUG-R12-13 剩余项。
 *
 * - 插件侧持有器 DateMappingBinding：不完整表拒绝、只接受一次、未绑定走路径 C。
 * - 真实模块导出：WtDataStorage / WtDataStorageAD 的两个读端绑定导出与 ParserXeleSkt 的解析器绑定导出。
 * - 宿主注入链：WtDtMgr::set_date_mapping 后经 WtDataStorageAD 读端读取 K 线，日期映射走注入的表（路径 A）。
 *   改前插件无导出，宿主找不到绑定函数，读端只走旧接口，计数表调用次数为 0（红控用改前读端模块）。
 */
#include "r71_wtcore_env.h"

#include <Includes/DateMappingBinding.h>
#include <Includes/IDataReader.h>
#include <Includes/IParserApi.h>
#include <Includes/IRdmDtReader.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <atomic>

using namespace wtp;

namespace {

/// 计数日期映射表：calc 原样回显日期、boundary 返回该日 09:00/15:00，其余未用条目返回 Unsupported。
struct K31CountingTable
{
	std::atomic<uint32_t> calc_calls{ 0 };
	std::atomic<uint32_t> boundary_calls{ 0 };
	DateMappingStatus calc_status = DateMappingStatus::Ok;

	DateMappingV1 table() const
	{
		DateMappingV1 t{};
		t.size = sizeof(DateMappingV1);
		t.version = 1;
		t.context = const_cast<K31CountingTable*>(this);
		t.calcTradingDate = [](const DateMappingV1* api, const char*, uint32_t d, uint32_t, uint8_t) noexcept {
			K31CountingTable* self = static_cast<K31CountingTable*>(api->context);
			self->calc_calls++;
			if (self->calc_status != DateMappingStatus::Ok)
				return DateMappingResult32{ self->calc_status, 0 };
			return DateMappingResult32{ DateMappingStatus::Ok, d };
		};
		t.getBoundaryTime = [](const DateMappingV1* api, const char*, uint32_t d, uint8_t, uint8_t start) noexcept {
			static_cast<K31CountingTable*>(api->context)->boundary_calls++;
			return DateMappingResult64{ DateMappingStatus::Ok, (uint64_t)d * 10000 + (start ? 900 : 1500) };
		};
		t.getTradingDate = [](const DateMappingV1*, const char*, uint32_t, uint32_t, uint8_t) noexcept {
			return DateMappingResult32{ DateMappingStatus::Unsupported, 0 };
		};
		t.nextTradingDate = t.getTradingDate;
		t.prevTradingDate = t.getTradingDate;
		return t;
	}
};

DateMappingV1 incomplete_table(const K31CountingTable& counter)
{
	DateMappingV1 t = counter.table();
	t.prevTradingDate = nullptr;	// 缺任一条目即不完整
	return t;
}

} // namespace

// —— 插件侧持有器 ——
TEST(S5MainK31DateBinding, BindingHolderValidatesOnceAndFallsBackToLegacy)
{
	K31CountingTable counter;
	DateMappingBinding binding;
	EXPECT_FALSE(binding.bound());

	const DateMappingV1 bad = incomplete_table(counter);
	EXPECT_FALSE(binding.bind(&bad));
	EXPECT_FALSE(binding.bind(nullptr));
	DateMappingV1 wrongSize = counter.table();
	wrongSize.size -= 1;
	EXPECT_FALSE(binding.bind(&wrongSize));
	EXPECT_FALSE(binding.bound());

	// 未绑定：路径 C，旧管理器为空时报 Unsupported，计数表不被调用
	{
		DateMappingClient client = binding.client(nullptr);
		EXPECT_EQ(DateMappingStatus::Unsupported, client.calcTradingDate("SHFE.ag", 20250110, 1000).status);
	}
	EXPECT_EQ(0u, counter.calc_calls.load());

	const DateMappingV1 good = counter.table();
	EXPECT_TRUE(binding.bind(&good));
	EXPECT_TRUE(binding.bound());
	K31CountingTable other;
	const DateMappingV1 second = other.table();
	EXPECT_FALSE(binding.bind(&second)) << "a second bind must be rejected and keep the first table";

	DateMappingClient client = binding.client(nullptr);
	const DateMappingResult32 r = client.calcTradingDate("SHFE.ag", 20250110, 1000);
	EXPECT_EQ(DateMappingStatus::Ok, r.status);
	EXPECT_EQ(20250110u, r.value);
	EXPECT_EQ(1u, counter.calc_calls.load());
	EXPECT_EQ(0u, other.calc_calls.load());
}

// —— 真实模块导出 ——
namespace {

template <typename Fn>
Fn k31_symbol(DllHandle h, const char* name)
{
	return reinterpret_cast<Fn>(DLLHelper::get_symbol(h, name));
}

} // namespace

TEST(S5MainK31DateBinding, StorageAndXeleModulesExportVersionedBinders)
{
	K31CountingTable counter;
	const DateMappingV1 good = counter.table();
	const DateMappingV1 bad = incomplete_table(counter);

	for (const char* module : { "WtDataStorage", "WtDataStorageAD" })
	{
		SCOPED_TRACE(module);
		r71::R71PluginDir plug(module);
		ASSERT_TRUE(plug.ok()) << "missing module " << module;
		DllHandle h = DLLHelper::load_library((plug.dir() + "/" + DLLHelper::wrap_module(module)).c_str());
		ASSERT_TRUE(h != NULL);

		auto createReader = k31_symbol<FuncCreateDataReader>(h, "createDataReader");
		auto deleteReader = k31_symbol<FuncDeleteDataReader>(h, "deleteDataReader");
		auto bindReader = k31_symbol<FuncBindDataReaderDateMappingV1>(h, WT_BIND_DATA_READER_DATE_MAPPING_V1);
		ASSERT_TRUE(createReader != NULL && deleteReader != NULL);
		ASSERT_TRUE(bindReader != NULL) << "data reader binder is not exported";
		IDataReader* reader = createReader();
		ASSERT_TRUE(reader != NULL);
		EXPECT_EQ(0, bindReader(NULL, &good));
		EXPECT_EQ(0, bindReader(reader, &bad));
		EXPECT_EQ(1, bindReader(reader, &good));
		EXPECT_EQ(0, bindReader(reader, &good));
		deleteReader(reader);

		auto createRdm = k31_symbol<FuncCreateRdmDtReader>(h, "createRdmDtReader");
		auto deleteRdm = k31_symbol<FuncDeleteRdmDtReader>(h, "deleteRdmDtReader");
		auto bindRdm = k31_symbol<FuncBindRdmDtReaderDateMappingV1>(h, WT_BIND_RDM_DT_READER_DATE_MAPPING_V1);
		ASSERT_TRUE(createRdm != NULL && deleteRdm != NULL);
		ASSERT_TRUE(bindRdm != NULL) << "random reader binder is not exported";
		IRdmDtReader* rdm = createRdm();
		ASSERT_TRUE(rdm != NULL);
		EXPECT_EQ(0, bindRdm(rdm, &bad));
		EXPECT_EQ(1, bindRdm(rdm, &good));
		EXPECT_EQ(0, bindRdm(rdm, &good));
		deleteRdm(rdm);
		// 模块保持加载到进程结束（与宿主一致，不在测试内卸载插件）。
	}

	{
		r71::R71PluginDir plug("ParserXeleSkt");
		ASSERT_TRUE(plug.ok()) << "missing module ParserXeleSkt";
		DllHandle h = DLLHelper::load_library((plug.dir() + "/" + DLLHelper::wrap_module("ParserXeleSkt")).c_str());
		ASSERT_TRUE(h != NULL);
		auto createParser = k31_symbol<FuncCreateParser>(h, "createParser");
		auto deleteParser = k31_symbol<FuncDeleteParser>(h, "deleteParser");
		auto bindParser = k31_symbol<FuncBindParserDateMappingV1>(h, WT_BIND_PARSER_DATE_MAPPING_V1);
		ASSERT_TRUE(createParser != NULL && deleteParser != NULL);
		ASSERT_TRUE(bindParser != NULL) << "parser binder is not exported";
		IParserApi* parser = createParser();
		ASSERT_TRUE(parser != NULL);
		EXPECT_EQ(0, bindParser(parser, &bad));
		EXPECT_EQ(1, bindParser(parser, &good));
		EXPECT_EQ(0, bindParser(parser, &good));
		deleteParser(parser);
		EXPECT_TRUE(parser == NULL);
	}
	EXPECT_EQ(0u, counter.calc_calls.load()) << "binding alone must not call the table";
}

// —— 宿主注入链：WtDtMgr → WtDataStorageAD 读端 ——
namespace {

class S5MainK31HostChain : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71BaseDataMgr bd;
	r71::R71HotMgr hot;
	r71::R71HftEngine engine;

	void SetUp() override
	{
		bd.regisSession("TRADING");
		WTSSessionInfo* sess = bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		WTSCommodityInfo* ag = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		ag->setSessionInfo(sess);
		bd.regis(ag, r71::makeCt("SHFE", "ag", "ag2512"));
		engine.attach(&bd, &hot, NULL);
	}

	WTSVariant* makeCfg()
	{
		std::string dir = r71::temp_root() + "k31_store_data/";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::string json = "{\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\"" + dir + "\"}}";
		return WTSCfgLoader::load_from_content(json.c_str());
	}
};

} // namespace

TEST_F(S5MainK31HostChain, InjectedTableDrivesTheReaderDateMapping)
{
	r71::R71PluginDir plug("WtDataStorageAD");
	ASSERT_TRUE(plug.ok()) << "missing libWtDataStorageAD";
	const std::string oldInst = WtHelper::getInstDir();
	WtHelper::setInstDir((plug.dir() + "/").c_str());

	K31CountingTable counter;
	{
		WtDtMgr mgr;
		mgr.set_date_mapping(counter.table());
		WTSVariant* cfg = makeCfg();
		ASSERT_NE(cfg, nullptr);
		ASSERT_TRUE(mgr.init(cfg, &engine, false));
		cfg->release();
		ASSERT_NE(mgr.reader(), nullptr);

		// 空数据根目录：读不到 K 线（NULL），但读取前的交易日映射必须经注入的表完成。
		EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 3, 202501101000ull) == NULL);
		EXPECT_GT(counter.calc_calls.load(), 0u) << "the reader did not use the host DateMappingV1 table";

		// 注入表报告缺日历时读取 fail-closed（路径 C 无法发现缺日历）。
		counter.calc_status = DateMappingStatus::MissingCalendar;
		const uint32_t before = counter.calc_calls.load();
		EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 3, 202501101000ull) == NULL);
		EXPECT_GT(counter.calc_calls.load(), before);
	}
	WtHelper::setInstDir(oldInst.c_str());
}

TEST_F(S5MainK31HostChain, WithoutHostTableTheReaderKeepsTheLegacyPath)
{
	r71::R71PluginDir plug("WtDataStorageAD");
	ASSERT_TRUE(plug.ok()) << "missing libWtDataStorageAD";
	const std::string oldInst = WtHelper::getInstDir();
	WtHelper::setInstDir((plug.dir() + "/").c_str());

	K31CountingTable counter;	// 未登记给宿主
	{
		WtDtMgr mgr;
		WTSVariant* cfg = makeCfg();
		ASSERT_NE(cfg, nullptr);
		ASSERT_TRUE(mgr.init(cfg, &engine, false));
		cfg->release();
		EXPECT_TRUE(mgr.get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 3, 202501101000ull) == NULL);
	}
	EXPECT_EQ(0u, counter.calc_calls.load());
	WtHelper::setInstDir(oldInst.c_str());
}
