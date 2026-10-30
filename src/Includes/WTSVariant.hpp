/*!
 * \file WTSVariant.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Wt通用变量对象定义
 */
#pragma once

#include "WTSTypes.h"
#include "WTSObject.hpp"
#include "WTSCollection.hpp"

#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <locale.h>
#include <sstream>
#include <string.h>
#include <limits>
#include <vector>
#include <map>

#ifdef _MSC_VER
#define INT64_FMT	"%I64d"
#define UINT64_FMT	"%I64u"
#else
#define INT64_FMT	"%ld"
#define UINT64_FMT	"%lu"
#endif


NS_WTP_BEGIN

/*
 *	WTSVariant是一个通用数据容器,设计目标是Json的Value类
 *	和Json不同的地方在于,WTSVariant满足WT系统内的派生关系
 *	可以通过引用计数管理数据,从而减少数据复制
 */
class WTSVariant : public WTSObject
{
public:
	typedef WTSArray					ChildrenArray;
	typedef WTSHashMap<std::string>		ChildrenMap;
	typedef std::vector<std::string>	MemberNames;

	typedef enum
	{
		VT_Null,
		VT_Array,
		VT_Int32,
		VT_Uint32,
		VT_Int64,
		VT_Uint64,
		VT_String,
		VT_Real,
		VT_Boolean,
		VT_Object
	}ValueType;


protected:
	WTSVariant() :_type(VT_Null){}

private:
	// Own the C numeric locale only for the active operation. Independent handles
	// avoid shared mutation, unload leaks, and static destruction-order hazards.
	class ScopedNumericCLocale
	{
	public:
		ScopedNumericCLocale()
#ifdef _WIN32
			: _locale(NULL)
#else
			: _locale((locale_t)0)
#endif
		{
#ifdef _WIN32
			_locale = _create_locale(LC_NUMERIC, "C");
#else
			_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
		}

		~ScopedNumericCLocale()
		{
#ifdef _WIN32
			if (_locale != NULL)
				_free_locale(_locale);
#else
			if (_locale != (locale_t)0)
				freelocale(_locale);
#endif
		}

		ScopedNumericCLocale(const ScopedNumericCLocale&) = delete;
		ScopedNumericCLocale& operator=(const ScopedNumericCLocale&) = delete;

		bool valid() const
		{
#ifdef _WIN32
			return _locale != NULL;
#else
			return _locale != (locale_t)0;
#endif
		}

		double parse(const char* text, char** end) const
		{
#ifdef _WIN32
			return _strtod_l(text, end, _locale);
#else
			return strtod_l(text, end, _locale);
#endif
		}

	private:
#ifdef _WIN32
		_locale_t _locale;
#else
		locale_t _locale;
#endif
	};

	static inline bool parseRealTextClassic(const std::string& text, double& value)
	{
		std::istringstream input(text);
		input.imbue(std::locale::classic());
		input >> value;
		if (input.bad())
			return false;

		// Some standard libraries report failbit for an otherwise valid subnormal
		// conversion. Keep the assigned value if the whole token was consumed.
		const bool subnormalRangeResult = input.fail() && input.eof() &&
			std::fpclassify(value) == FP_SUBNORMAL;
		if (input.fail() && !subnormalRangeResult)
			return false;
		if (subnormalRangeResult)
			input.clear(input.rdstate() & ~std::ios::failbit);
		input >> std::ws;
		return input.eof() && std::isfinite(value);
	}

	static inline bool parseRealText(const std::string& text, double& value, const ScopedNumericCLocale& cLocale)
	{
		if (text == "nan" || text == "+nan" || text == "-nan")
		{
			value = std::numeric_limits<double>::quiet_NaN();
			if (text == "-nan")
				value = std::copysign(value, -1.0);
			return true;
		}
		if (text == "inf" || text == "+inf")
		{
			value = std::numeric_limits<double>::infinity();
			return true;
		}
		if (text == "-inf")
		{
			value = -std::numeric_limits<double>::infinity();
			return true;
		}
		if (text.empty())
			return false;

		const char* begin = text.c_str();
		char* end = NULL;
		if (!cLocale.valid())
			return parseRealTextClassic(text, value);
		value = cLocale.parse(begin, &end);
		return end == begin + text.size() && std::isfinite(value);
	}

	static inline std::string formatReal(double value)
	{
		if (std::isnan(value))
			return std::signbit(value) ? "-nan" : "nan";
		if (std::isinf(value))
			return std::signbit(value) ? "-inf" : "inf";
		if (value == 0.0)
			return std::signbit(value) ? "-0" : "0";

		// Use only C++11-era locale-aware streams here: floating charconv is not
		// available in every supported C++17 library (notably GCC 8's libstdc++).
		// Reducing significant digits until the C-locale parser round-trips keeps
		// the stored text short without relying on the process locale.
		ScopedNumericCLocale cLocale;
		std::string fallback;
		for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10; ++precision)
		{
			std::ostringstream output;
			output.imbue(std::locale::classic());
			output << std::defaultfloat << std::setprecision(precision) << value;
			if (!output)
				continue;

			std::string candidate = output.str();
			fallback = candidate;
			double roundTrip = 0.0;
			if (parseRealText(candidate, roundTrip, cLocale) && roundTrip == value)
				return candidate;
		}
		return fallback;
	}

	static inline int32_t realToInt32(double value)
	{
		// Truncate in-range values toward zero and saturate at the destination
		// bounds; NaN maps to zero. This avoids undefined floating-to-integer casts.
		if (std::isnan(value))
			return 0;
		const double limit = std::ldexp(1.0, std::numeric_limits<int32_t>::digits);
		if (value <= -limit)
			return std::numeric_limits<int32_t>::min();
		if (value >= limit)
			return std::numeric_limits<int32_t>::max();
		return static_cast<int32_t>(value);
	}

	static inline uint32_t realToUInt32(double value)
	{
		// Negative values and NaN map to zero; positive overflow saturates.
		if (!(value > 0.0))
			return 0;
		const double limit = std::ldexp(1.0, std::numeric_limits<uint32_t>::digits);
		if (value >= limit)
			return std::numeric_limits<uint32_t>::max();
		return static_cast<uint32_t>(value);
	}

	static inline int64_t realToInt64(double value)
	{
		// Truncate in-range values toward zero and saturate at the destination
		// bounds; NaN maps to zero. This avoids undefined floating-to-integer casts.
		if (std::isnan(value))
			return 0;
		const double limit = std::ldexp(1.0, std::numeric_limits<int64_t>::digits);
		if (value <= -limit)
			return std::numeric_limits<int64_t>::min();
		if (value >= limit)
			return std::numeric_limits<int64_t>::max();
		return static_cast<int64_t>(value);
	}

	static inline uint64_t realToUInt64(double value)
	{
		// Negative values and NaN map to zero; positive overflow saturates.
		if (!(value > 0.0))
			return 0;
		const double limit = std::ldexp(1.0, std::numeric_limits<uint64_t>::digits);
		if (value >= limit)
			return std::numeric_limits<uint64_t>::max();
		return static_cast<uint64_t>(value);
	}

	static inline WTSVariant* create(int32_t i32)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Int32;
		ret->_value._string = new std::string(std::to_string(i32));
		return ret;
	}

	static inline WTSVariant* create(uint32_t u32)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Uint32;
		ret->_value._string = new std::string(std::to_string(u32));
		return ret;
	}

	static inline WTSVariant* create(int64_t i64)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Int64;
		ret->_value._string = new std::string(std::to_string(i64));
		return ret;
	}

	static inline WTSVariant* create(uint64_t u64)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Uint64;
		ret->_value._string = new std::string(std::to_string(u64));
		return ret;
	}

	static inline WTSVariant* create(double _real)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Real;
		ret->_value._string = new std::string(formatReal(_real));
		return ret;
	}

	static inline WTSVariant* create(const char* _string)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_String;
		ret->_value._string = new std::string(_string);
		return ret;
	}

	static inline WTSVariant* create(bool _bool)
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Boolean;
		ret->_value._string = new std::string(_bool ? "true" : "false");
		return ret;
	}

public:
	static inline WTSVariant* createObject()
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Object;
		ret->_value._map = ChildrenMap::create();
		return ret;
	}

	static inline WTSVariant* createArray()
	{
		WTSVariant* ret = new WTSVariant();
		ret->_type = VT_Array;
		ret->_value._array = ChildrenArray::create();
		return ret;
	}

	inline bool has(const char* key) const
	{
		if (_type != VT_Object)
			return false;

		auto it = _value._map->find(key);
		if (it == _value._map->end())
			return false;

		return true;
	}

	inline int32_t asInt32() const
	{
		switch (_type)
		{
		case VT_Null:
			return 0;
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
			return _value._string ? (int32_t)atof(_value._string->c_str()) : 0;
		case VT_Real:
			return _value._string ? realToInt32(asDouble()) : 0;
		default:
			return 0;
		}
	}

	inline uint32_t asUInt32() const
	{
		switch (_type)
		{
		case VT_Null:
			return 0;
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
			return _value._string ? (uint32_t)atof(_value._string->c_str()) : 0;
		case VT_Real:
			return _value._string ? realToUInt32(asDouble()) : 0;
		default:
			return 0;
		}
	}

	inline int64_t asInt64() const
	{
		switch (_type)
		{
		case VT_Null:
			return 0;
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
			return _value._string ? strtoll(_value._string->c_str(), NULL, 10) : 0;
		case VT_Real:
			return _value._string ? realToInt64(asDouble()) : 0;
		default:
			return 0;
		}
	}

	inline uint64_t asUInt64() const
	{
		switch (_type)
		{
		case VT_Null:
			return 0;
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
			return _value._string ? strtoull(_value._string->c_str(), NULL, 10) : 0;
		case VT_Real:
			return _value._string ? realToUInt64(asDouble()) : 0;
		default:
			return 0;
		}
	}

	inline double asDouble() const
	{
		switch (_type)
		{
		case VT_Null:
			return 0.0;
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
			return _value._string ? strtod(_value._string->c_str(), NULL) : 0.0;
		case VT_Real:
			if (!_value._string)
				return 0.0;
			{
				ScopedNumericCLocale cLocale;
				double value = 0.0;
				return parseRealText(*_value._string, value, cLocale) ? value : 0.0;
			}
		default:
			return 0.0;
		}
	}

	inline std::string	asString() const
	{
		switch (_type)
		{
		case VT_Null:
			return "";
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_Real:
		case VT_String:
		case VT_Boolean:
			return _value._string ? *_value._string : "";
		default:
			return "";
		}
	}

	inline const char* asCString() const
	{
		if (_type != VT_Object && _type != VT_Array && _value._string != NULL)
			return _value._string->c_str();

		return "";
	}

	inline bool asBoolean() const
	{
		switch (_type)
		{
		case VT_Int32:
		case VT_Uint32:
		case VT_Int64:
		case VT_Uint64:
		case VT_String:
		case VT_Real:
		case VT_Boolean:
			if (_value._string)
				return wt_stricmp(_value._string->c_str(), "true") == 0 || wt_stricmp(_value._string->c_str(), "yes") == 0;
			return false;
		case VT_Null:
		case VT_Array:
		case VT_Object:
		default:
			// Container and null boolean conversion is unspecified; false is a safe fallback.
			return false;
		}
	}

	inline int32_t getInt32(const char* name, int32_t defVal = 0) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asInt32();

		return defVal;
	}

	inline uint32_t getUInt32(const char* name, uint32_t defVal = 0) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asUInt32();

		return defVal;
	}

	inline int64_t getInt64(const char* name, int64_t defVal = 0) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asInt64();

		return defVal;
	}

	inline uint64_t getUInt64(const char* name, uint64_t defVal = 0) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asUInt64();

		return defVal;
	}

	inline double getDouble(const char* name, double defVal = 0) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asDouble();

		return defVal;
	}

	inline std::string getString(const char* name, const char* defVal = "") const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asString();

		return defVal;
	}

	inline const char* getCString(const char* name, const char* defVal = "") const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asCString();

		return defVal;
	}

	inline bool getBoolean(const char* name, bool defVal = false) const
	{
		WTSVariant* p = get(name);
		if (p)
			return p->asBoolean();

		return defVal;
	}

	inline WTSVariant* get(const char* name) const
	{
		if (_type != VT_Object)
			return NULL;

		if (_value._map == NULL)
			return NULL;

		WTSVariant* ret = static_cast<WTSVariant*>(_value._map->get(name));
		return ret;
	}

	inline WTSVariant* get(const std::string& name) const
	{
		if (_type != VT_Object)
			return NULL;

		if (_value._map == NULL)
			return NULL;

		WTSVariant* ret = static_cast<WTSVariant*>(_value._map->get(name));
		return ret;
	}

	inline WTSVariant* get(uint32_t idx) const
	{
		if (_type != VT_Array)
			return NULL;

		if (_value._array == NULL)
			return NULL;

		WTSVariant* ret = static_cast<WTSVariant*>(_value._array->at(idx));
		return ret;
	}

	inline bool append(const char* _name, const char* _string)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_string);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, int32_t _i32)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_i32);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, uint32_t _u32)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_u32);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, int64_t _i64)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_i64);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, uint64_t _u64)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_u64);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, double _real)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_real);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, bool _bool)
	{
		if (_type != VT_Object)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		WTSVariant* item = WTSVariant::create(_bool);
		_value._map->add(_name, item, false);
		//item->release();

		return true;
	}

	inline bool append(const char* _name, WTSVariant *item, bool bAutoRetain = true)
	{
		if (_type != VT_Object || NULL == item)
			return false;

		if (_value._map == NULL)
		{
			_value._map = ChildrenMap::create();
		}

		_value._map->add(_name, item, bAutoRetain);

		return true;
	}

	inline bool append(const char* _str)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_str);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(int32_t _i32)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_i32);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(uint32_t _u32)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_u32);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(int64_t _i64)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_i64);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(uint64_t _u64)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_u64);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(double _real)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_real);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(bool _bool)
	{
		if (_type != VT_Array)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		WTSVariant* item = WTSVariant::create(_bool);
		_value._array->append(item, false);
		//item->release();

		return true;
	}

	inline bool append(WTSVariant *item, bool bAutoRetain = true)
	{
		if (_type != VT_Array || NULL == item)
			return false;

		if (_value._array == NULL)
		{
			_value._array = ChildrenArray::create();
		}

		_value._array->append(item, bAutoRetain);

		return true;
	}

	inline uint32_t size() const
	{
		if (_type != VT_Array && _type != VT_Object)
			return 0;

		else if (_type == VT_Array)
		{
			return (_value._array == NULL) ? 0 : _value._array->size();
		}
		else
		{
			return (_value._map == NULL) ? 0 : _value._map->size();
		}
	}

	inline MemberNames memberNames() const
	{
		MemberNames names;
		if (_type == VT_Object && _value._map != NULL)
		{
			auto it = _value._map->begin();
			for (; it != _value._map->end(); it++)
			{
				names.emplace_back(it->first);
			}
		}

		return names;
	}

	virtual void release()
	{
		if (isSingleRefs())
		{
			switch (_type)
			{
			case VT_Array:
				if (NULL != _value._array)
				{
					_value._array->release();
				}
				break;
			case VT_Object:
				if (NULL != _value._map)
				{
					_value._map->release();
				}
				break;
			default:
				if (NULL != _value._string)
				{
					delete _value._string;
				}
				break;
			}
		}
		WTSObject::release();
	}

	inline ValueType type() const{ return _type; }

	inline bool isArray() const{ return _type == VT_Array; }
	inline bool isObject() const{ return _type == VT_Object; }

private:
	union ValueHolder
	{
		std::string*	_string;

		ChildrenMap*	_map;
		ChildrenArray*	_array;
	};
	ValueHolder	_value;
	ValueType	_type;
};

NS_WTP_END
