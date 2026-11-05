/*
 * t26_spi_access.hpp — CTraderSpi 的测试友元（CTraderSpi 内 `friend struct CTraderSpiQueueTestAccess;`）
 *
 * 同一测试二进制内 dump/flow 两个 TU 都要用，定义只能有一份（ODR），因此从 flow 用例中移出到本头文件。
 *
 * 迭代03 卡14 14a（I03-NEW-093）：QRYFEES 模式下回调只接受本批次经 BeginFeeQuery 登记、类型一致且
 * 未终态的请求号；批次在合约查询终帧（sealed）且所有请求“已发送 + 已确认”后才收口落盘。
 * 下面的辅助函数按生产 worker 的同一登记路径（BeginFeeQuery + FinishFeeSubmission）登记请求，
 * 只省去 watchdog/队列限速，回报仍走真实回调与收口逻辑。
 */
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "CTPLoader/TraderSpi.h"

struct CTraderSpiQueueTestAccess
{
	static void load_from_json(CTraderSpi& spi)
	{
		spi.LoadFromJson();
	}

	static bool wait_until_stopped(CTraderSpi& spi, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(spi._mtx);
		return spi._query_ready.wait_for(lock, timeout, [&spi]() { return spi._stopped; });
	}

	/// 开启费率批次并登记一条“已发送”的合约查询，返回请求号（0 表示被拒）。
	static int begin_instrument_query(CTraderSpi& spi)
	{
		{
			std::lock_guard<std::mutex> lock(spi._mtx);
			spi._fee_batch_active = true;
		}
		const int id = spi.BeginFeeQuery(CTraderSpi::FeeQueryKind::Instrument);
		if (id != 0)
			spi.FinishFeeSubmission(id, 0);
		return id;
	}

	/// 把合约查询记为终态并封口批次，但不触发合约终帧对费率查询的入队（dump 白盒用例使用）。
	static void seal_instrument_query(CTraderSpi& spi, int id)
	{
		std::lock_guard<std::mutex> lock(spi._mtx);
		auto it = spi._fee_requests.find(id);
		if (it != spi._fee_requests.end())
			it->second.terminal = true;
		spi._fee_batch_sealed = true;
	}

	/// 为指定合约登记一条“已发送”的佣金（commission=true）或保证金查询，返回请求号。
	static int begin_rate_query(CTraderSpi& spi, bool commission, const loader::Contract& contract)
	{
		const int id = spi.BeginFeeQuery(commission ? CTraderSpi::FeeQueryKind::Commission
			: CTraderSpi::FeeQueryKind::Margin, &contract);
		if (id != 0)
			spi.FinishFeeSubmission(id, 0);
		return id;
	}
};
