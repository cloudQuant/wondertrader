/*!
 * \file WtBtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测引擎运行器实现
 * \details 本文件实现了WonderTrader回测引擎的独立运行器，用于执行各类策略的回测测试
 * 支持CTA策略、SEL选股策略、HFT高频策略、UFT策略和Exec执行器策略的回测
 */
#include <iostream>
#include "../WtBtCore/HisDataReplayer.h"
#include "../WtBtCore/CtaMocker.h"
#include "../WtBtCore/ExecMocker.h"
#include "../WtBtCore/HftMocker.h"
#include "../WtBtCore/SelMocker.h"
#include "../WtBtCore/UftMocker.h"
#include "../WtBtCore/WtHelper.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/SignalHook.hpp"

#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/cppcli.hpp"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#endif

/**
 * @brief 回测引擎的主函数
 * @details 实现了回测引擎的入口点，包括命令行参数解析、配置文件加载、回测环境初始化、
 * 策略实例创建和注册、回测执行等功能
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return int 程序返回值，0表示成功，非0表示出错
 */
int main(int argc, char* argv[])
{

#ifdef _MSC_VER
    CMiniDumper::Enable("WtBtRunner.exe", true, WtHelper::getCWD().c_str());
#endif
	//std::cout << "WtBtRunner begins" << std::endl;
	fmt::print("---WtBtRunner begins---\n");
	cppcli::Option opt(argc, argv);

	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgbt.yaml as default", false);

	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	opt.parse();

	if (hParam->exists())
		return 0;

	std::string filename;
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfgbt.yaml";
	WTSLogger::init(filename.c_str());
	//std::cout << " logger initialized" << std::endl;
	fmt::print("---logger initialized---\n");
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	});

	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./configbt.yaml";

	if (!StdFile::exists(filename.c_str()))
	{
		fmt::print("confiture {} not exists", filename);
		return 0;
	}

	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename.c_str());
	if (cfg == NULL)
	{
		WTSLogger::info("Loading configuration file {} failed", filename);
		return -1;
	}

	HisDataReplayer replayer;
	replayer.init(cfg->get("replayer"));

	WTSVariant* cfgEnv = cfg->get("env");
	const char* mode = cfgEnv->getCString("mocker");
	int32_t slippage = cfgEnv->getInt32("slippage");
	//std::cout << "cfg initialized" << std::endl;
	fmt::print("---cfg initialized---\n");

	if (strcmp(mode, "cta") == 0)
	{
		fmt::print("---cta initialized---\n");
		CtaMocker* mocker = new CtaMocker(&replayer, "cta", slippage);
		fmt::print("---mocker initialized---\n");
		mocker->init_cta_factory(cfg->get("cta"));
		//mocker->init_cta_factory(cfg);
		fmt::print("---cta_factory initialized---\n");
		const char* stra_id = cfg->get("cta")->get("strategy")->getCString("id");
		fmt::print("---stra_id = {} initialized---\n", stra_id);
		// 加载增量回测的基础历史回测数据
		const char* incremental_backtest_base = cfg->get("env")->getCString("incremental_backtest_base");
		fmt::print("---incremental_backtest_base initialized---\n");
		if (strlen(incremental_backtest_base) > 0)
		{
			mocker->load_incremental_data(incremental_backtest_base);
		}
		fmt::print("---load_incremental_data ends---\n");
		replayer.register_sink(mocker, stra_id);
		fmt::print("---cta ends---\n");
	}
	else if (strcmp(mode, "hft") == 0)
	{
		HftMocker* mocker = new HftMocker(&replayer, "hft");
		mocker->init_hft_factory(cfg->get("hft"));
		const char* stra_id = cfg->get("hft")->get("strategy")->getCString("id");
		replayer.register_sink(mocker, stra_id);
	}
	else if (strcmp(mode, "sel") == 0)
	{
		SelMocker* mocker = new SelMocker(&replayer, "sel", slippage);
		mocker->init_sel_factory(cfg->get("sel"));
		const char* stra_id = cfg->get("sel")->get("strategy")->getCString("id");
		replayer.register_sink(mocker, stra_id);

		replayer.register_task(mocker->id(), cfg->get("sel")->get("task")->getUInt32("date"),
			cfg->get("sel")->get("task")->getUInt32("time"), cfg->get("sel")->get("task")->getCString("period"));
	}
	else if (strcmp(mode, "exec") == 0)
	{
		ExecMocker* mocker = new ExecMocker(&replayer);
		mocker->init(cfg->get("exec"));
		replayer.register_sink(mocker, "exec");
	}
	else if (strcmp(mode, "uft") == 0)
	{
		UftMocker* mocker = new UftMocker(&replayer, "uft");
		mocker->init_uft_factory(cfg->get("uft"));
		const char* stra_id = cfg->get("uft")->get("strategy")->getCString("id");
		replayer.register_sink(mocker, stra_id);
	}

	replayer.prepare();

	replayer.run(true);

	printf("press enter key to exit\r\n");
	getchar();

	WTSLogger::stop();
}
