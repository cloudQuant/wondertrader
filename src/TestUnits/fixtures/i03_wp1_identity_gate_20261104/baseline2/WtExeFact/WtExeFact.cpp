/*!
 * \file WtExeFact.cpp
 * \brief 执行单元工厂类实现文件
 *
 * 该文件实现了执行单元工厂类(WtExeFact)，负责创建、枚举和管理各种类型的执行单元
 * 
 * \author Wesley
 * \date 2020/03/30
 */
#include "WtExeFact.h"

#include "WtTWapExeUnit.h"
#include "WtMinImpactExeUnit.h"
#include "WtDiffMinImpactExeUnit.h"
#include "WtStockMinImpactExeUnit.h"
#include "WtVWapExeUnit.h"
#include "WtStockVWapExeUnit.h"

#include "../Includes/IExecPluginV2.h"

#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_set>
/**
 * @brief 工厂名称常量
 */
const char* FACT_NAME = "WtExeFact";

namespace
{
	std::mutex g_v2_init_mutex;
	std::unordered_set<ExecuteUnit*> g_v2_init_attempted;

	void set_error(char* error_text, uint32_t capacity, const char* message)
	{
		if (error_text == NULL || capacity == 0)
			return;

		const size_t length = std::strlen(message);
		const size_t copy_length = (length < capacity - 1) ? length : capacity - 1;
		if (copy_length != 0)
			std::memcpy(error_text, message, copy_length);
		error_text[copy_length] = '\0';
	}

	void* WT_EXEC_PLUGIN_V2_CALL create_factory_v2()
	{
		try
		{
			return new WtExeFact();
		}
		catch (...)
		{
			return NULL;
		}
	}

	void WT_EXEC_PLUGIN_V2_CALL destroy_factory_v2(void* factory)
	{
		try
		{
			delete static_cast<WtExeFact*>(factory);
		}
		catch (...)
		{
			// No C++ exception may escape a plugin callback.
		}
	}

	const char* WT_EXEC_PLUGIN_V2_CALL get_factory_name_v2(void* factory)
	{
		if (factory == NULL)
			return NULL;

		try
		{
			return static_cast<WtExeFact*>(factory)->getName();
		}
		catch (...)
		{
			return NULL;
		}
	}

	void* WT_EXEC_PLUGIN_V2_CALL create_unit_v2(void* factory, WtExecPluginV2UnitKind kind, const char* unit_name)
	{
		if (factory == NULL || unit_name == NULL || unit_name[0] == '\0')
			return NULL;

		try
		{
			WtExeFact* fact = static_cast<WtExeFact*>(factory);
			switch (kind)
			{
			case WT_EXEC_PLUGIN_V2_UNIT_PLAIN:
				return fact->createExeUnit(unit_name);
			case WT_EXEC_PLUGIN_V2_UNIT_DIFF:
				return fact->createDiffExeUnit(unit_name);
			case WT_EXEC_PLUGIN_V2_UNIT_ARBI:
				return fact->createArbiExeUnit(unit_name);
			default:
				return NULL;
			}
		}
		catch (...)
		{
			return NULL;
		}
	}

	void WT_EXEC_PLUGIN_V2_CALL destroy_unit_v2(void* factory, void* unit)
	{
		if (unit == NULL)
			return;

		try
		{
			std::lock_guard<std::mutex> guard(g_v2_init_mutex);
			g_v2_init_attempted.erase(static_cast<ExecuteUnit*>(unit));
		}
		catch (...)
		{
			// Continue with destruction even if init-state bookkeeping fails.
		}

		try
		{
			if (factory != NULL)
			{
				if (static_cast<WtExeFact*>(factory)->deleteExeUnit(static_cast<ExecuteUnit*>(unit)))
					return;
			}
			delete static_cast<ExecuteUnit*>(unit);
		}
		catch (...)
		{
			// Destruction callbacks are noexcept across the C ABI boundary.
		}
	}

	WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL initialize_unit_v2(
		void* factory, void* unit, void* execute_context, const char* std_code,
		void* configuration, char* error_text, uint32_t error_capacity)
	{
		ExecuteUnit* exec_unit = NULL;
		bool initialization_started = false;
		set_error(error_text, error_capacity, "");
		if (factory == NULL || unit == NULL || execute_context == NULL ||
			std_code == NULL || std_code[0] == '\0' || configuration == NULL)
		{
			set_error(error_text, error_capacity, "invalid execution-unit initialization arguments");
			return WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT;
		}

		try
		{
			WtExeFact* fact = static_cast<WtExeFact*>(factory);
			exec_unit = static_cast<ExecuteUnit*>(unit);
			if (std::strcmp(fact->getName(), FACT_NAME) != 0 ||
				std::strcmp(exec_unit->getFactName(), FACT_NAME) != 0)
			{
				set_error(error_text, error_capacity, "factory or unit does not belong to WtExeFact");
				return WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT;
			}

			const bool supported =
				dynamic_cast<WtTWapExeUnit*>(exec_unit) != NULL ||
				dynamic_cast<WtMinImpactExeUnit*>(exec_unit) != NULL ||
				dynamic_cast<WtDiffMinImpactExeUnit*>(exec_unit) != NULL ||
				dynamic_cast<WtStockMinImpactExeUnit*>(exec_unit) != NULL ||
				dynamic_cast<WtVWapExeUnit*>(exec_unit) != NULL ||
				dynamic_cast<WtStockVWapExeUnit*>(exec_unit) != NULL;
			if (!supported)
			{
				set_error(error_text, error_capacity, "unsupported execution-unit implementation");
				return WT_EXEC_PLUGIN_V2_UNSUPPORTED_KIND;
			}

			{
				std::lock_guard<std::mutex> guard(g_v2_init_mutex);
				if (!g_v2_init_attempted.insert(exec_unit).second)
				{
					set_error(error_text, error_capacity, "execution-unit initialization was already attempted");
					return WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
				}
			}

			// The host calls this callback once. Keep initialization in one place so
			// the status below reflects the same init invocation that it just ran.
			initialization_started = true;
			exec_unit->init(static_cast<ExecuteContext*>(execute_context), std_code,
				static_cast<WTSVariant*>(configuration));

			bool initialized = false;
			if (WtTWapExeUnit* typed = dynamic_cast<WtTWapExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();
			else if (WtMinImpactExeUnit* typed = dynamic_cast<WtMinImpactExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();
			else if (WtDiffMinImpactExeUnit* typed = dynamic_cast<WtDiffMinImpactExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();
			else if (WtStockMinImpactExeUnit* typed = dynamic_cast<WtStockMinImpactExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();
			else if (WtVWapExeUnit* typed = dynamic_cast<WtVWapExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();
			else if (WtStockVWapExeUnit* typed = dynamic_cast<WtStockVWapExeUnit*>(exec_unit))
				initialized = typed->initializationSucceeded();

			if (!initialized)
			{
				exec_unit->clearBorrowedContext();
				set_error(error_text, error_capacity, "execution-unit configuration was rejected");
				return WT_EXEC_PLUGIN_V2_INVALID_CONFIGURATION;
			}
			return WT_EXEC_PLUGIN_V2_OK;
		}
		catch (...)
		{
			if (initialization_started && exec_unit != NULL)
				exec_unit->clearBorrowedContext();
			set_error(error_text, error_capacity, "execution-unit initialization raised an exception");
			return WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
		}
	}

	// The built-in WtExeFact units do not create worker threads or asynchronous
	// tasks of their own. Their callbacks run on threads owned by the host, so
	// this only acknowledges plugin-owned work; the host must quiesce its pool.
	WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL request_quiesce_v2(void* factory, void* unit)
	{
		return factory != NULL && unit != NULL
			? WT_EXEC_PLUGIN_V2_OK
			: WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT;
	}

	int32_t WT_EXEC_PLUGIN_V2_CALL can_wait_quiesce_v2(void* factory, void* unit)
	{
		return factory != NULL && unit != NULL ? 1 : 0;
	}

	int32_t WT_EXEC_PLUGIN_V2_CALL wait_quiesce_v2(void* factory, void* unit)
	{
		return factory != NULL && unit != NULL ? 1 : 0;
	}

	int32_t WT_EXEC_PLUGIN_V2_CALL can_wait_factory_quiesce_v2(void* factory)
	{
		// WtExeFact has no plugin-owned workers, including during unit init.
		return factory != NULL ? 1 : 0;
	}

	const WtExecPluginApiV2 EXEC_PLUGIN_API_V2 = {
		sizeof(WtExecPluginApiV2),
		WT_EXEC_PLUGIN_V2_ABI_VERSION,
		{ 0, 0 },
		create_factory_v2,
		destroy_factory_v2,
		get_factory_name_v2,
		create_unit_v2,
		destroy_unit_v2,
		initialize_unit_v2,
		WT_EXEC_PLUGIN_V2_CAP_QUIESCE |
			WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT,
		request_quiesce_v2,
		can_wait_quiesce_v2,
		wait_quiesce_v2,
		can_wait_factory_quiesce_v2
	};
}

/**
 * @brief C接口函数区
 */
extern "C"
{
	EXPORT_FLAG const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL getExecPluginApiV2(uint32_t requested_major)
	{
		if (requested_major != WT_EXEC_PLUGIN_V2_ABI_MAJOR)
			return NULL;
		return &EXEC_PLUGIN_API_V2;
	}

	/**
	 * @brief 创建执行单元工厂的导出函数
	 * @return 新创建的执行单元工厂实例
	 */
	EXPORT_FLAG IExecuterFact* createExecFact()
	{
		IExecuterFact* fact = new WtExeFact();
		return fact;
	}

	/**
	 * @brief 删除执行单元工厂的导出函数
	 * @param fact 要删除的执行单元工厂实例
	 */
	EXPORT_FLAG void deleteExecFact(IExecuterFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
};

static_assert(std::is_same<decltype(&createExecFact), FuncCreateExeFact>::value,
	"createExecFact must exactly match the legacy factory function typedef");
static_assert(std::is_same<decltype(&deleteExecFact), FuncDeleteExeFact>::value,
	"deleteExecFact must exactly match the legacy factory deleter typedef");


/**
 * @brief 构造函数
 */
WtExeFact::WtExeFact()
{
}


/**
 * @brief 析构函数
 */
WtExeFact::~WtExeFact()
{
}

/**
 * @brief 获取工厂名称
 * @return 返回工厂名称常量
 */
const char* WtExeFact::getName()
{
	return FACT_NAME;
}

/**
 * @brief 枚举所有支持的执行单元
 * @details 通过回调函数将所支持的执行单元列表提供给上层应用
 * @param cb 回调函数，包含工厂名称、单元名称和是否是特殊单元信息
 */
void WtExeFact::enumExeUnit(FuncEnumUnitCallback cb)
{
	cb(FACT_NAME, "WtTWapExeUnit", false);
	cb(FACT_NAME, "WtMinImpactExeUnit", true);
}

/**
 * @brief 创建普通执行单元
 * @details 根据指定的名称创建相应类型的执行单元实例
 * @param name 执行单元名称，支持WtTWapExeUnit, WtMinImpactExeUnit等
 * @return 创建的执行单元指针，如果不支持该名称则返回NULL
 */
ExecuteUnit* WtExeFact::createExeUnit(const char* name)
{
	if (strcmp(name, "WtTWapExeUnit") == 0)
		return new WtTWapExeUnit();
	else if (strcmp(name, "WtMinImpactExeUnit") == 0)
		return new WtMinImpactExeUnit();
	else if (strcmp(name, "WtStockMinImpactExeUnit") == 0)
		return new WtStockMinImpactExeUnit();
	else if (strcmp(name, "WtVWapExeUnit") == 0)
		return  new WtVWapExeUnit();
	else if (strcmp(name, "WtStockVWapExeUnit") == 0)
		return new WtStockVWapExeUnit();
	return NULL;
}

/**
 * @brief 创建差量执行单元
 * @details 根据指定的名称创建差量执行单元实例，当前仅支持WtDiffMinImpactExeUnit
 * @param name 执行单元名称
 * @return 创建的差量执行单元指针，如果不支持该名称则返回NULL
 */
ExecuteUnit* WtExeFact::createDiffExeUnit(const char* name)
{
	if (strcmp(name, "WtDiffMinImpactExeUnit") == 0)
		return new WtDiffMinImpactExeUnit();

	return NULL;
}

/**
 * @brief 创建套利执行单元
 * @details 根据指定的名称创建套利执行单元实例，当前暂不支持任何套利执行单元
 * @param name 执行单元名称
 * @return 创建的套利执行单元指针，当前始终返回NULL
 */
ExecuteUnit* WtExeFact::createArbiExeUnit(const char* /*name*/)
{
	return NULL;
}

/**
 * @brief 删除执行单元
 * @details 对指定的执行单元进行安全删除，首先检查单元是否为NULL和是否由本工厂创建
 * @param unit 要删除的执行单元指针
 * @return 删除是否成功：true-成功删除或单元为NULL，false-单元不属于本工厂创建
 */
bool WtExeFact::deleteExeUnit(ExecuteUnit* unit)
{
	if (unit == NULL)
		return true;

	if (strcmp(unit->getFactName(), FACT_NAME) != 0)
		return false;

	delete unit;
	return true;
}
