#include "WtBlkIdxWorker.h"

#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"

#include "../Share/CodeHelper.hpp"
#include "../Share/decimal.h"
#include "../Share/TimeUtils.hpp"

const char* WEIGHT_ALGS[] = 
{
	"Fixed",
	"DynamicInterest",
	"DynamicVolume"
};

namespace
{
	bool is_decimal_digit(char value)
	{
		return value >= '0' && value <= '9';
	}

	bool parse_weight_alg_string(const std::string& raw, uint32_t& weightAlg)
	{
		std::size_t pos = 0;
		bool negative = false;
		if (pos < raw.size() && (raw[pos] == '+' || raw[pos] == '-'))
		{
			negative = raw[pos] == '-';
			++pos;
		}

		std::string digits;
		while (pos < raw.size() && is_decimal_digit(raw[pos]))
			digits.push_back(raw[pos++]);

		std::size_t fractionalDigits = 0;
		if (pos < raw.size() && raw[pos] == '.')
		{
			++pos;
			while (pos < raw.size() && is_decimal_digit(raw[pos]))
			{
				digits.push_back(raw[pos++]);
				++fractionalDigits;
			}
		}
		if (digits.empty())
			return false;

		bool negativeExponent = false;
		std::size_t exponentMagnitude = 0;
		if (pos < raw.size() && (raw[pos] == 'e' || raw[pos] == 'E'))
		{
			++pos;
			if (pos < raw.size() && (raw[pos] == '+' || raw[pos] == '-'))
			{
				negativeExponent = raw[pos] == '-';
				++pos;
			}

			const std::size_t exponentLimit = raw.size();
			bool hasExponentDigits = false;
			while (pos < raw.size() && is_decimal_digit(raw[pos]))
			{
				hasExponentDigits = true;
				const std::size_t digit = static_cast<std::size_t>(raw[pos++] - '0');
				if (exponentMagnitude > exponentLimit / 10 ||
					(exponentMagnitude == exponentLimit / 10 && digit > exponentLimit % 10))
					exponentMagnitude = exponentLimit;
				else
					exponentMagnitude = exponentMagnitude * 10 + digit;
			}
			if (!hasExponentDigits)
				return false;
		}
		if (pos != raw.size())
			return false;

		const std::size_t firstNonZero = digits.find_first_not_of('0');
		if (firstNonZero == std::string::npos)
		{
			weightAlg = 0;
			return true;
		}
		digits.erase(0, firstNonZero);
		if (negative)
			return false;

		std::size_t scale = 0;
		if (negativeExponent)
		{
			if (fractionalDigits >= digits.size() || exponentMagnitude >= digits.size() - fractionalDigits)
				return false;
			scale = fractionalDigits + exponentMagnitude;
		}
		else if (exponentMagnitude >= fractionalDigits)
		{
			if (exponentMagnitude != fractionalDigits)
				return false;
		}
		else
		{
			scale = fractionalDigits - exponentMagnitude;
		}

		if (scale >= digits.size())
			return false;
		if (scale > 0)
		{
			const std::size_t integerEnd = digits.size() - scale;
			for (std::size_t i = integerEnd; i < digits.size(); ++i)
			{
				if (digits[i] != '0')
					return false;
			}
			digits.resize(integerEnd);
		}

		if (digits == "1")
			weightAlg = 1;
		else if (digits == "2")
			weightAlg = 2;
		else
			return false;
		return true;
	}

	bool parse_supported_weight_alg(WTSVariant* config, uint32_t& weightAlg)
	{
		WTSVariant* value = config->get("weight_alg");
		if (value == NULL)
		{
			// Preserve the old getUInt32 default when the optional field is absent.
			weightAlg = 0;
			return true;
		}

		switch (value->type())
		{
		case WTSVariant::VT_Int32:
		{
			const int32_t raw = value->asInt32();
			if (raw < 0 || raw > 2)
				return false;
			weightAlg = static_cast<uint32_t>(raw);
			return true;
		}
		case WTSVariant::VT_Uint32:
		{
			const uint32_t raw = value->asUInt32();
			if (raw > 2)
				return false;
			weightAlg = raw;
			return true;
		}
		case WTSVariant::VT_Int64:
		{
			const int64_t raw = value->asInt64();
			if (raw < 0 || raw > 2)
				return false;
			weightAlg = static_cast<uint32_t>(raw);
			return true;
		}
		case WTSVariant::VT_Uint64:
		{
			const uint64_t raw = value->asUInt64();
			if (raw > 2)
				return false;
			weightAlg = static_cast<uint32_t>(raw);
			return true;
		}
		case WTSVariant::VT_Real:
		{
			const double raw = value->asDouble();
			if (raw == 0.0 || raw == 1.0 || raw == 2.0)
			{
				weightAlg = static_cast<uint32_t>(raw);
				return true;
			}
			return false;
		}
		case WTSVariant::VT_String:
		{
			// Preserve exact decimal/scientific integer strings without floating-point rounding.
			return parse_weight_alg_string(value->asString(), weightAlg);
		}
		default:
			return false;
		}
	}
}

extern const char* FACT_NAME;

const char* WtBlkIdxWorker::get_fact_name()
{
	return FACT_NAME;
}

bool WtBlkIdxWorker::init(WTSVariant* config)
{
	if (config == NULL)
		return false;

	// Validate before resolving contracts or subscribing to constituent ticks.
	uint32_t weightAlg = 0;
	if (!parse_supported_weight_alg(config, weightAlg))
	{
		if (_factory != NULL)
			_factory->output_log(LL_ERROR, fmtutil::format("[{}] Invalid block-index weight algorithm; expected 0, 1, or 2", _id));
		return false;
	}
	_weight_alg = weightAlg;

	_exchg = config->getCString("exchg");
	_code = config->getCString("code");

	_cInfo = _bd_mgr->getContract(_code.c_str(), _exchg.c_str());

	_trigger = config->getCString("trigger");
	_timeout = config->getUInt32("timeout");

	_stand_scale = config->getDouble("stand_scale");	//标准化系数，如计算值为3000，标准化以后为1000，则标准化系数为0.333333
	if (decimal::eq(_stand_scale, 0.0))
		_stand_scale = 1.0;

	//如果是连续合约，要转成分月合约，因为是实时处理的
	IHotMgr* hotMgr = _hot_mgr;

	if (_trigger != "time")
	{
		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(_trigger.c_str(), hotMgr);
		if (strlen(cInfo._ruletag) > 0)
			_trigger = fmt::format("{}.{}", cInfo._exchg, hotMgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID()));
	}

	WTSVariant* cfgComms = config->get("commodities");
	WTSVariant* cfgCodes = config->get("codes");
	if (cfgComms != NULL && cfgComms->size() > 0)
	{
		IBaseDataMgr* bdMgr = _bd_mgr;

		std::size_t cnt = cfgComms->size();
		for (std::size_t i = 0; i < cnt; i++)
		{
			WTSVariant* cfgItem = cfgComms->get(i);
			std::string fullPid;
			double weight = 1.0;

			//如果是对象，则会有scale，作为权重缩放系数，这种主要用于多品种板块，不同品种权重不同
			if (cfgItem->isObject())
			{
				fullPid = cfgItem->getCString("code");
				weight = cfgItem->getDouble("weight");
				if (decimal::eq(weight, 0.0))
					weight = 1.0;
			}
			else
			{
				fullPid = cfgItem->asCString();
			}

			//通过品种代码找到对应的合约列表，并订阅
			WTSCommodityInfo* commInfo = bdMgr->getCommodity(fullPid.c_str());
			if (commInfo == NULL)
				continue;

			const auto& codes = commInfo->getCodes();
			for (const auto& c : codes)
			{
				std::string fullCode = fmt::format("{}.{}", commInfo->getExchg(), c.c_str());
				WeightFactor& wFactor = _weight_scales[fullCode];
				wFactor._weight = weight;

				//订阅的时候读取最后的快照，作为基础数据
				WTSTickData* lastTick = _factory->sub_ticks(fullCode.c_str());
				if (lastTick)
				{
					memcpy(&wFactor._tick, &lastTick->getTickStruct(), sizeof(WTSTickStruct));
					lastTick->release();
				}

				_factory->output_log(LL_INFO, fmtutil::format("[{}] Consist {} of block index {}.{} subscribed", _id, fullCode, _exchg, _code));
			}
		}
	}
	else if (cfgCodes != NULL && cfgCodes->size() > 0)
	{
		[[maybe_unused]] IBaseDataMgr* bdMgr = _bd_mgr;
		IHotMgr* hotMgr = _hot_mgr;

		std::size_t cnt = cfgCodes->size();
		for (std::size_t i = 0; i < cnt; i++)
		{
			WTSVariant* cfgItem = cfgCodes->get(i);
			std::string fullCode;
			double weight = 1.0;

			//如果是对象，则会有scale，作为权重缩放系数，这种主要用于多品种板块，不同品种权重不同
			if (cfgItem->isObject())
			{
				std::string fullPid = cfgItem->getCString("code");
				weight = cfgItem->getDouble("weight");
				if (decimal::eq(weight, 0.0))
					weight = 1.0;
			}
			else
			{
				fullCode = cfgItem->asCString();
			}

			auto ay = StrUtil::split(fullCode, ".");
			std::string exchg = ay[0];
			std::string code;
			if(ay.size() == 2)
			{
				//这是fullcode格式，即CFFEX.IF2202
				code = ay[1];
			}
			else
			{
				//大于2，就是stdCode格式，主要考虑CFFEX.IF.HOT
				CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(fullCode.c_str(), hotMgr);
				if (strlen(cInfo._ruletag) > 0)
				{
					code = hotMgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID());
					_factory->output_log(LL_INFO, fmtutil::format("[{}] {} contract confirmed: {} -> {}.{}", _id, cInfo._ruletag, fullCode, cInfo._exchg, code));
				}
				else
				{
					code = cInfo._code;
				}
				fullCode = fmt::format("{}.{}", cInfo._exchg, code);
				
			}

			WTSContractInfo* cInfo = _bd_mgr->getContract(code.c_str(), exchg.c_str());
			if(cInfo == NULL)
			{
				_factory->output_log(LL_ERROR, fmtutil::format("[{}] Consist {} of block index {}.{} not exists", _id, fullCode, _exchg, _code));
				continue;
			}

			WeightFactor& wFactor = _weight_scales[fullCode];
			wFactor._weight = weight;

			//订阅的时候读取最后的快照，作为基础数据
			WTSTickData* lastTick = _factory->sub_ticks(fullCode.c_str());
			if (lastTick)
			{
				memcpy(&wFactor._tick, &lastTick->getTickStruct(), sizeof(WTSTickStruct));
				lastTick->release();
			}

			_factory->output_log(LL_INFO, fmtutil::format("[{}] Consist {} of block index {}.{} subscribed", _id, fullCode, _exchg, _code));
		}
	}

	_factory->output_log(LL_INFO, fmtutil::format("[{}] Block index {}.{} initialized，weight algorithm: {}, trigger: {}, timeout: {}", _id, _exchg, _code, WEIGHT_ALGS[_weight_alg], _trigger, _timeout));

	return true;
}

void WtBlkIdxWorker::handle_quote(WTSTickData* newTick)
{
	const char* fullCode = newTick->getContractInfo()->getFullCode();

	{
		SpinLock lock(_mtx_data);
		auto it = _weight_scales.find(fullCode);
		if (it == _weight_scales.end())
			return;

		WeightFactor& wFactor = (WeightFactor&)it->second;
		memcpy(&wFactor._tick, &newTick->getTickStruct(), sizeof(WTSTickStruct));
	}

	//如果使用time，那么当第一个成分合约的行情进来以后，会去更新指数重算时间
	if(_trigger != "time" && _trigger.compare(fullCode) != 0)
		return;

	//如果是_trigger，则开始准备触发了
	if(_timeout == 0)
	{
		//如果没有设置超时时间，则直接生成指数
		generate_tick();
	}
	else
	{
		if(_thrd_trigger == NULL)
		{
			_thrd_trigger.reset(new StdThread([this]() {
				while(!_stopped)
				{
					while(!_process)
					{
						StdUniqueLock lck(_mtx_trigger);
						_cond_trigger.wait(_mtx_trigger);
					}

					//等待一段时间
					do
					{
						uint64_t now = TimeUtils::getLocalTimeNow();
						if(now >= _recalc_time)
							break;

						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					} while (true);

					//开始生成指数
					generate_tick();
					_process = false;
				}
			}));
		}

		if(!_process)
		{
			_process = true;
			_recalc_time = TimeUtils::getLocalTimeNow() + _timeout;
			_cond_trigger.notify_all();
		}
	}
}

void WtBlkIdxWorker::generate_tick()
{
	//然后开始计算指数
	double total_base = 0.0;	//权重基数
	double total_value = 0.0;	//数值累加
	double total_vol = 0.0;		//指数总成交量
	double total_amt = 0.0;		//指数总成交额
	double total_hold = 0.0;	//指数总持
	uint64_t maxTime = 0;		//最后一笔tick的时间
	uint32_t tDate = 0;			//交易日

	double total_weight = 0;

	{
		//先把数据锁住
		SpinLock lock(_mtx_data);
		for (const auto& v : _weight_scales)
		{
			//如果数据不全，直接退出
			const WeightFactor& wFactor = v.second;
			if (wFactor._tick.action_date == 0)
				return;

			uint64_t curTime = TimeUtils::makeTime(wFactor._tick.action_date, wFactor._tick.action_time);
			maxTime = std::max(maxTime, curTime);
			tDate = std::max(tDate, wFactor._tick.trading_date);

			total_vol += wFactor._tick.total_volume;
			total_amt += wFactor._tick.total_turnover;
			total_hold += wFactor._tick.open_interest;

			total_weight += wFactor._weight;

			switch (_weight_alg)
			{
			case 0://固定权重，只看本身的weight
				total_base = 1;	//因为只看本身weight，所以权重基数为1
				total_value += wFactor._tick.price * wFactor._weight;
				break;
			case 1:	//动态总持
				total_base += wFactor._tick.open_interest;	//动态总持为当前总持
				total_value += wFactor._tick.open_interest * wFactor._tick.price * wFactor._weight;
				break;
			case 2:	//动态成交量
				total_base += wFactor._tick.total_volume;	//动态成交量
				total_value += wFactor._tick.total_volume * wFactor._tick.price * wFactor._weight;
				break;
			default:
				break;
			}
		}
	}

	// Dynamic weighting needs a non-zero open-interest or volume base. A
	// zero-activity snapshot has no defined index price, so do not publish it.
	// Leave the cache untouched; a later valid snapshot can still initialize it.
	if ((_weight_alg == 1 || _weight_alg == 2) && total_base == 0.0)
		return;

	//数据做标准化
	double index = total_value / total_base / total_weight * _stand_scale;

	//时间做一个修正
	maxTime += _timeout;
	TimeUtils::Time32 tm32(maxTime);

	if(_cache.action_time == 0)
	{
		strcpy(_cache.exchg, _exchg.c_str());
		strcpy(_cache.code, _code.c_str());
		_cache.trading_date = tDate;

		_cache.price = index;
		_cache.open = index;
		_cache.high = index;
		_cache.low = index;		

		_cache.action_date = tm32.date();
		_cache.action_time = tm32.time_ms();
		
		_cache.total_volume = total_vol;
		_cache.open_interest = total_hold;
		_cache.total_turnover = total_amt;
	}
	else
	{
		_cache.price = index;
		_cache.high = std::max(_cache.high, index);
		_cache.low = std::min(_cache.low, index);

		_cache.action_date = tm32.date();
		_cache.action_time = tm32.time_ms();

		_cache.total_volume = total_vol;
		_cache.open_interest = total_hold;
		_cache.total_turnover = total_amt;
	}

	WTSTickData *newTick = WTSTickData::create(_cache);
	newTick->setContractInfo(_cInfo);
	_factory->push_tick(newTick);
	newTick->release();
}
