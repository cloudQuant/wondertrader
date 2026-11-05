/*!
 * \file TickerDayBoundary.h
 * \project	WonderTrader
 *
 * \brief 实时 ticker 的日界辅助与事件串行化（迭代03）
 *
 * 卡3 3a=A（2026-11-04 用户裁定）：
 * - 收盘分钟标签保留 2400（全天交易时段的最后一分钟闭合为旧日 2400）。
 * - 行情 action_time 必须是合法的 HHMMSSmmm：HH<24、MM<60、SS<60。
 *   23:59:60 等闰秒/越界时刻属于非法输入，ticker 拒绝，不推进任何时间状态。
 * - 行情交易日新于引擎当前交易日时视为新交易日首 tick：先闭合旧日未发布的分钟，
 *   再把分钟位置归零，以新日日期/时间推进引擎，并把该 tick 交给引擎（不丢首 tick）。
 *
 * 卡9 9d=A / R202-001 ticker 合同（2026-11-03 卡9 确认，2026-11-04 R202 明确同 9d=A）：
 * - 每个 ticker（CTA/SEL/HFT）的 on_tick 与 worker 通过 EventStrand 串行执行：
 *   任一时刻只有一个线程是 owner，ticker 状态只在 owner 内读写。
 * - 在 owner 线程内（策略/引擎/数据回调中）同步重入 on_tick 不就地执行，而是按 FIFO 排队，
 *   在当前事件返回后的安全点由同一 owner 依次投递，每个排队事件恰好投递一次。
 * - 其他线程调用 on_tick 时等待当前 owner 完成（含排队事件）后再执行。
 * - stop：先发停止请求并唤醒 worker；非 owner 调用方在有界超时内等待当前事件与队列排空，
 *   再 join worker；worker 线程（回调内）调用 stop 只发请求，不自 join。
 */
#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "../Includes/WTSObject.hpp"

namespace wt_ticker
{
	inline bool isValidTickTime(uint32_t uTime)
	{
		const uint32_t hhmmss = uTime / 1000;
		const uint32_t hh = hhmmss / 10000;
		const uint32_t mm = (hhmmss / 100) % 100;
		const uint32_t ss = hhmmss % 100;
		return hh < 24 && mm < 60 && ss < 60;
	}

	inline bool isNewTradingDate(uint32_t curPos, uint32_t tickTDate, uint32_t engineTDate)
	{
		return curPos != 0 && tickTDate != 0 && tickTDate > engineTDate;
	}

	//stop 等待当前事件与排队事件排空的上限
	constexpr std::chrono::milliseconds STOP_DRAIN_TIMEOUT{ 5000 };

	class EventStrand
	{
	public:
		struct Item
		{
			wtp::WTSObject*	obj = nullptr;
			uint32_t		flag = 0;
		};

		enum class Entry { Owner, Queued };

		EventStrand() = default;
		EventStrand(const EventStrand&) = delete;
		EventStrand& operator=(const EventStrand&) = delete;

		~EventStrand()
		{
			for (Item& item : _queue)
				if (item.obj != nullptr)
					item.obj->release();
		}

		//on_tick 入口：owner 线程同步重入时排队（持有引用），否则等待成为 owner
		Entry enterEvent(wtp::WTSObject* obj, uint32_t flag)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			const std::thread::id self = std::this_thread::get_id();
			if (_busy && _owner == self)
			{
				if (obj != nullptr)
					obj->retain();
				Item item;
				item.obj = obj;
				item.flag = flag;
				_queue.push_back(item);
				return Entry::Queued;
			}
			_cv.wait(lock, [this]() { return !_busy; });
			_busy = true;
			_owner = self;
			return Entry::Owner;
		}

		//worker 入口：已请求停止时返回 false 且不成为 owner
		bool enterWorker(const std::atomic<bool>& stopped)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this, &stopped]() { return !_busy || stopped.load(std::memory_order_acquire); });
			if (stopped.load(std::memory_order_acquire))
				return false;
			_busy = true;
			_owner = std::this_thread::get_id();
			return true;
		}

		//owner 调用：取下一个排队事件；队列为空时原子地释放 owner 并返回 false
		bool nextOrLeave(Item& out)
		{
			std::lock_guard<std::mutex> lock(_mtx);
			if (!_queue.empty())
			{
				out = _queue.front();
				_queue.pop_front();
				return true;
			}
			_busy = false;
			_owner = std::thread::id();
			_cv.notify_all();
			return false;
		}

		//异常路径：丢弃排队事件并释放 owner
		void abandon()
		{
			std::deque<Item> dropped;
			{
				std::lock_guard<std::mutex> lock(_mtx);
				dropped.swap(_queue);
				_busy = false;
				_owner = std::thread::id();
				_cv.notify_all();
			}
			for (Item& item : dropped)
				if (item.obj != nullptr)
					item.obj->release();
		}

		bool isOwner() const
		{
			std::lock_guard<std::mutex> lock(_mtx);
			return _busy && _owner == std::this_thread::get_id();
		}

		std::size_t queued() const
		{
			std::lock_guard<std::mutex> lock(_mtx);
			return _queue.size();
		}

		void notifyAll()
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_cv.notify_all();
		}

		//等待没有 owner 且队列为空；超时返回 false
		bool waitIdle(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			return _cv.wait_for(lock, timeout, [this]() { return !_busy && _queue.empty(); });
		}

		//worker 休眠：停止请求可立即唤醒
		void sleepFor(std::chrono::milliseconds duration, const std::atomic<bool>& stopped)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait_for(lock, duration, [&stopped]() { return stopped.load(std::memory_order_acquire); });
		}

	private:
		mutable std::mutex		_mtx;
		std::condition_variable	_cv;
		bool					_busy = false;
		std::thread::id			_owner;
		std::deque<Item>		_queue;
	};

	/*
	 *	调用方已是 owner：执行 body，然后按 FIFO 投递排队事件（handler(obj, flag)），最后释放 owner。
	 *	body/handler 内的同步重入会继续排在队尾，由本循环投递。
	 */
	template<typename Body, typename Handler>
	void runAndDrain(EventStrand& strand, Body&& body, Handler&& handler)
	{
		try
		{
			body();
			EventStrand::Item item;
			while (strand.nextOrLeave(item))
			{
				struct Releaser
				{
					wtp::WTSObject* obj;
					~Releaser() { if (obj != nullptr) obj->release(); }
				} releaser{ item.obj };
				handler(item.obj, item.flag);
			}
		}
		catch (...)
		{
			strand.abandon();
			throw;
		}
	}
}
