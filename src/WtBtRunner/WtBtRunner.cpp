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
#include <Share/cppcli.hpp>
#include <string>

#ifdef _MSC_VER
#include "../Common/mdump.h"
#endif

namespace
{
	bool quiesce_exec_mocker(ExecMocker* mocker, std::string* error)
	{
		if (!mocker)
			return true;
		if (!mocker->canStopFromCurrentThread())
		{
			if (error) *error = "execution plugin preflight rejected the current control thread";
			return false;
		}
		return mocker->requestQuiesce(error) &&
			mocker->canWaitQuiesce(error) &&
			mocker->waitQuiesce(error);
	}
}

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
		filename = "./logcfgdt.yaml";
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
		fmt::print("Configuration file {} does not exist\n", filename);
		return 1;
	}

	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename.c_str());
	if (cfg == NULL)
	{
		WTSLogger::info("Loading configuration file {} failed", filename);
		return -1;
	}

	WTSVariant* cfgEnv = cfg->get("env");
	const char* mode = cfgEnv && cfgEnv->isObject()
		? cfgEnv->getCString("mocker", NULL)
		: NULL;
	auto reject_mocker_config = [cfg](const std::string& message) {
		WTSLogger::error("{}", message);
		cfg->release();
		WTSLogger::stop();
		return -1;
	};
	if (!mode || mode[0] == '\0')
		return reject_mocker_config("Backtest configuration requires env.mocker");
	if (strcmp(mode, "cta") != 0 && strcmp(mode, "hft") != 0 && strcmp(mode, "sel") != 0 &&
		strcmp(mode, "exec") != 0 && strcmp(mode, "uft") != 0)
		return reject_mocker_config(std::string("Unsupported backtest mocker mode: ") + mode);
	const int32_t slippage = cfgEnv->getInt32("slippage");

	HisDataReplayer* replayer = new HisDataReplayer();
	if (!replayer->init(cfg->get("replayer")))
	{
		WTSLogger::error("Initializing historical data replayer failed");
		cfg->release();
		delete replayer;
		WTSLogger::stop();
		return -1;
	}
	ExecMocker* execMocker = NULL;

	//std::cout << "cfg initialized" << std::endl;
	fmt::print("---cfg initialized---\n");

	if (strcmp(mode, "cta") == 0)
	{
		fmt::print("---cta initialized---\n");
		CtaMocker* mocker = new CtaMocker(replayer, "cta", slippage);
		fmt::print("---mocker initialized---\n");
		if (!mocker->init_cta_factory(cfg->get("cta")))
		{
			delete mocker;
			WTSLogger::error("Initializing CTA strategy factory failed");
			cfg->release();
			delete replayer;
			WTSLogger::stop();
			return -1;
		}
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
		replayer->register_sink(mocker, stra_id);
		fmt::print("---cta ends---\n");
	}
	else if (strcmp(mode, "hft") == 0)
	{
		HftMocker* mocker = new HftMocker(replayer, "hft");
		if (!mocker->init_hft_factory(cfg->get("hft")))
		{
			delete mocker;
			WTSLogger::error("Initializing HFT strategy factory failed");
			cfg->release();
			delete replayer;
			WTSLogger::stop();
			return -1;
		}
		const char* stra_id = cfg->get("hft")->get("strategy")->getCString("id");
		replayer->register_sink(mocker, stra_id);
	}
	else if (strcmp(mode, "sel") == 0)
	{
		WTSVariant* selConfig = cfg->get("sel");
		WTSVariant* taskConfig = selConfig ? selConfig->get("task") : NULL;
		const char* taskPeriod = taskConfig ? taskConfig->getCString("period") : NULL;
		// The standalone CLI historically registers only the period and uses these defaults.
		const char* taskTradingTemplate = "CHINA";
		const char* taskSession = "TRADING";
		if (!HisDataReplayer::validate_task_parameters(taskPeriod, taskTradingTemplate, taskSession))
		{
			WTSLogger::error("Invalid SEL task configuration");
			cfg->release();
			delete replayer;
			WTSLogger::stop();
			return -1;
		}

		SelMocker* mocker = new SelMocker(replayer, "sel", slippage);
		if (!mocker->init_sel_factory(selConfig))
		{
			delete mocker;
			WTSLogger::error("Initializing SEL strategy factory failed");
			cfg->release();
			delete replayer;
			WTSLogger::stop();
			return -1;
		}
		const char* stra_id = selConfig->get("strategy")->getCString("id");
		replayer->register_sink(mocker, stra_id);

		replayer->register_task(mocker->id(), taskConfig->getUInt32("date"),
			taskConfig->getUInt32("time"), taskPeriod, taskTradingTemplate, taskSession);
	}
	else if (strcmp(mode, "exec") == 0)
	{
		execMocker = new ExecMocker(replayer);
		std::string initError;
		if (!execMocker->init(cfg->get("exec"), &initError))
		{
			std::string quiesceError;
			if (quiesce_exec_mocker(execMocker, &quiesceError))
			{
				delete execMocker;
				execMocker = NULL;
				cfg->release();
				delete replayer;
				WTSLogger::stop();
			}
			else
			{
				WTSLogger::error("Execution mocker initialization failed and cleanup is pending: {} ({})",
					initError, quiesceError);
				// Retain the plugin, replay context, and configuration until process
				// exit; unwinding these owners would invalidate a live ExecuteContext.
			}
			return -1;
		}
		replayer->register_sink(execMocker, "exec");
	}
	else if (strcmp(mode, "uft") == 0)
	{
		UftMocker* mocker = new UftMocker(replayer, "uft");
		mocker->init_uft_factory(cfg->get("uft"));
		const char* stra_id = cfg->get("uft")->get("strategy")->getCString("id");
		replayer->register_sink(mocker, stra_id);
	}

	if (!replayer->prepare())
	{
		// I03-NEW-019：输出回放器记录的错误码与消息，便于定位 prepare 失败原因
		const WtReplayStatusV1 status = replayer->replay_status();
		WTSLogger::error("Preparing historical data replayer failed: replay error {}: {}",
			status.error_code, status.message[0] != '\0' ? status.message : "no message");
		if (execMocker)
		{
			std::string quiesceError;
			if (!quiesce_exec_mocker(execMocker, &quiesceError))
			{
				WTSLogger::error("Execution plugin cleanup is pending; replay owners are retained: {}", quiesceError);
				return -1;
			}
			delete execMocker;
			execMocker = NULL;
		}
		cfg->release();
		delete replayer;
		WTSLogger::stop();
		return -1;
	}

	replayer->run(true);

	printf("press enter key to exit\r\n");
	getchar();
	if (execMocker)
	{
		std::string quiesceError;
		if (!quiesce_exec_mocker(execMocker, &quiesceError))
		{
			WTSLogger::error("Execution plugin cleanup is pending; replay owners are retained: {}", quiesceError);
			return -1;
		}
		delete execMocker;
		cfg->release();
		delete replayer;
	}
	else
	{
		cfg->release();
		delete replayer;
	}

	WTSLogger::stop();
	return 0;
}
