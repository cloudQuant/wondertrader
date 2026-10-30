// Lane R12 - src/Includes/WTSVariant.hpp 变体类型读写
// 覆盖：createObject/createArray + 全部 append 重载、按名字/下标读取、缺失键取默认值、
//       类型不符时的换算（asInt32/asUInt32/asInt64/asUInt64/asDouble/asString/asCString）、
//       同键覆盖的引用计数变化、size/memberNames/has/type 判定
// 说明：标量 create() 是私有工厂，只能由 append(name, value) 间接创建（与生产用法一致）。
#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <string>
#include <utility>
#include <vector>

#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace {
typedef WTSVariant::MemberNames Names;

class CommaDecimalPoint : public std::numpunct<char>
{
protected:
	char do_decimal_point() const override { return ','; }
};

class ScopedGlobalLocale
{
public:
	explicit ScopedGlobalLocale(const std::locale& locale) : _previous(std::locale::global(locale)) {}
	~ScopedGlobalLocale() { std::locale::global(_previous); }

private:
	std::locale _previous;
};

class ScopedCNumericLocale
{
public:
	ScopedCNumericLocale()
	{
		const char* current = std::setlocale(LC_NUMERIC, nullptr);
		if (current != nullptr)
			_previous = current;
	}

	~ScopedCNumericLocale()
	{
		if (!_previous.empty())
			std::setlocale(LC_NUMERIC, _previous.c_str());
	}

	bool trySet(const char* name)
	{
		return std::setlocale(LC_NUMERIC, name) != nullptr;
	}

private:
	std::string _previous;
};
}

TEST(r12_variant_object, append_and_read_each_scalar_type)
{
	WTSVariant* obj = WTSVariant::createObject();
	ASSERT_NE(obj, nullptr);
	EXPECT_TRUE(obj->isObject());
	EXPECT_FALSE(obj->isArray());
	EXPECT_EQ(obj->type(), WTSVariant::VT_Object);
	EXPECT_EQ(obj->size(), 0u);

	EXPECT_TRUE(obj->append("i32", (int32_t)-42));
	EXPECT_TRUE(obj->append("u32", (uint32_t)4000000000u));
	EXPECT_TRUE(obj->append("i64", (int64_t)-9000000000LL));
	EXPECT_TRUE(obj->append("u64", (uint64_t)9000000000ULL));
	EXPECT_TRUE(obj->append("real", 3.14159));
	EXPECT_TRUE(obj->append("str", "hello"));
	EXPECT_TRUE(obj->append("yes", true));
	EXPECT_TRUE(obj->append("no", false));
	EXPECT_EQ(obj->size(), 8u);

	WTSVariant* v = obj->get("i32");
	ASSERT_NE(v, nullptr);
	EXPECT_EQ(v->type(), WTSVariant::VT_Int32);
	EXPECT_EQ(v->asInt32(), -42);
	EXPECT_EQ(v->asString(), std::string("-42"));
	// 有符号负值按无符号读会得到回绕结果，因此只断言正值分支
	EXPECT_EQ(obj->getInt32("i32"), -42);
	EXPECT_EQ(obj->getUInt32("u32"), 4000000000u);
	EXPECT_EQ(obj->getInt64("i64"), -9000000000LL);
	EXPECT_EQ(obj->getUInt64("u64"), 9000000000ULL);

	WTSVariant* rv = obj->get("real");
	EXPECT_EQ(rv->type(), WTSVariant::VT_Real);
	// Real values use a shortest round-trip decimal representation.
	EXPECT_EQ(rv->asString(), std::string("3.14159"));
	EXPECT_DOUBLE_EQ(rv->asDouble(), 3.14159);
	// 实数截断成整数
	EXPECT_EQ(rv->asInt32(), 3);
	EXPECT_EQ(obj->getDouble("real"), 3.14159);

	WTSVariant* sv = obj->get("str");
	EXPECT_EQ(sv->type(), WTSVariant::VT_String);
	EXPECT_STREQ(sv->asCString(), "hello");
	EXPECT_EQ(sv->asString(), std::string("hello"));
	// 非数字字符串换算成 0，而不是默认值
	EXPECT_EQ(sv->asInt32(), 0);
	EXPECT_EQ(obj->getInt32("str", 99), 0);
	EXPECT_EQ(obj->getString("str", "d"), std::string("hello"));
	EXPECT_STREQ(obj->getCString("str", "d"), "hello");

	// 布尔：true/false/yes 三种写法都认，其它为 false
	EXPECT_TRUE(obj->get("yes")->asBoolean());
	EXPECT_TRUE(obj->getBoolean("yes"));
	EXPECT_FALSE(obj->get("no")->asBoolean());
	EXPECT_FALSE(obj->getBoolean("no"));
	obj->append("yn", "yes");
	EXPECT_TRUE(obj->getBoolean("yn"));
	obj->append("n2", "NO");
	EXPECT_FALSE(obj->getBoolean("n2"));
	obj->release();
}

TEST(r12_variant_real, object_and_array_preserve_double_round_trip)
{
	const std::vector<std::pair<const char*, double>> values = {
		{"near_one", 1.00000000001},
		{"tiny", 1e-11},
		{"ordinary", 12345.678901234567},
		{"subnormal", std::numeric_limits<double>::denorm_min()},
		{"largest", std::numeric_limits<double>::max()},
		{"positive_zero", 0.0},
		{"negative_zero", -0.0},
		{"positive_infinity", std::numeric_limits<double>::infinity()},
		{"negative_infinity", -std::numeric_limits<double>::infinity()},
		{"nan", std::numeric_limits<double>::quiet_NaN()}
	};

	WTSVariant* object = WTSVariant::createObject();
	WTSVariant* array = WTSVariant::createArray();
	ASSERT_NE(object, nullptr);
	ASSERT_NE(array, nullptr);
	for (const auto& sample : values)
	{
		ASSERT_TRUE(object->append(sample.first, sample.second));
		ASSERT_TRUE(array->append(sample.second));
	}

	for (size_t i = 0; i < values.size(); ++i)
	{
		const double expected = values[i].second;
		WTSVariant* objectValue = object->get(values[i].first);
		WTSVariant* arrayValue = array->get(static_cast<uint32_t>(i));
		ASSERT_NE(objectValue, nullptr);
		ASSERT_NE(arrayValue, nullptr);
		if (std::isnan(expected))
		{
			EXPECT_TRUE(std::isnan(objectValue->asDouble()));
			EXPECT_TRUE(std::isnan(arrayValue->asDouble()));
		}
		else
		{
			EXPECT_EQ(expected, objectValue->asDouble());
			EXPECT_EQ(expected, arrayValue->asDouble());
		}
		if (expected == 0.0)
		{
			EXPECT_EQ(std::signbit(expected), std::signbit(objectValue->asDouble()));
			EXPECT_EQ(std::signbit(expected), std::signbit(arrayValue->asDouble()));
		}
	}
	EXPECT_NE("0", object->get("tiny")->asString());
	EXPECT_NE("1", object->get("near_one")->asString());
	EXPECT_EQ("-0", object->get("negative_zero")->asString());
	EXPECT_EQ("inf", object->get("positive_infinity")->asString());
	EXPECT_EQ("-inf", object->get("negative_infinity")->asString());

	object->release();
	array->release();
}

TEST(r12_variant_real, integer_accessors_truncate_and_saturate_safely)
{
	WTSVariant* object = WTSVariant::createObject();
	WTSVariant* array = WTSVariant::createArray();
	ASSERT_NE(object, nullptr);
	ASSERT_NE(array, nullptr);

	const double int32Limit = std::ldexp(1.0, std::numeric_limits<int32_t>::digits);
	const double uint32Limit = std::ldexp(1.0, std::numeric_limits<uint32_t>::digits);
	const double int64Limit = std::ldexp(1.0, std::numeric_limits<int64_t>::digits);
	const double uint64Limit = std::ldexp(1.0, std::numeric_limits<uint64_t>::digits);
	ASSERT_TRUE(object->append("tiny", 1e-5));
	ASSERT_TRUE(object->append("million", 1e6));
	ASSERT_TRUE(object->append("fraction", 12345.875));
	ASSERT_TRUE(object->append("negative_fraction", -12345.875));
	ASSERT_TRUE(object->append("int32_upper", int32Limit));
	ASSERT_TRUE(object->append("int32_lower", -int32Limit));
	ASSERT_TRUE(object->append("uint32_upper", uint32Limit));
	ASSERT_TRUE(object->append("int64_upper", int64Limit));
	ASSERT_TRUE(object->append("int64_lower", -int64Limit));
	ASSERT_TRUE(object->append("uint64_upper", uint64Limit));
	ASSERT_TRUE(object->append("positive_infinity", std::numeric_limits<double>::infinity()));
	ASSERT_TRUE(object->append("negative_infinity", -std::numeric_limits<double>::infinity()));
	ASSERT_TRUE(object->append("nan", std::numeric_limits<double>::quiet_NaN()));

	EXPECT_EQ(0, object->get("tiny")->asInt32());
	EXPECT_EQ(0u, object->get("tiny")->asUInt32());
	EXPECT_EQ(0LL, object->get("tiny")->asInt64());
	EXPECT_EQ(0ULL, object->get("tiny")->asUInt64());
	EXPECT_EQ(1000000, object->get("million")->asInt32());
	EXPECT_EQ(1000000u, object->get("million")->asUInt32());
	EXPECT_EQ(1000000LL, object->get("million")->asInt64());
	EXPECT_EQ(1000000ULL, object->get("million")->asUInt64());
	EXPECT_EQ(12345, object->get("fraction")->asInt32());
	EXPECT_EQ(12345u, object->get("fraction")->asUInt32());
	EXPECT_EQ(12345LL, object->get("fraction")->asInt64());
	EXPECT_EQ(12345ULL, object->get("fraction")->asUInt64());
	EXPECT_EQ(-12345, object->get("negative_fraction")->asInt32());
	EXPECT_EQ(-12345LL, object->get("negative_fraction")->asInt64());
	EXPECT_EQ(0u, object->get("negative_fraction")->asUInt32());
	EXPECT_EQ(0ULL, object->get("negative_fraction")->asUInt64());
	EXPECT_EQ(std::numeric_limits<int32_t>::max(), object->get("int32_upper")->asInt32());
	EXPECT_EQ(2147483648u, object->get("int32_upper")->asUInt32());
	EXPECT_EQ(std::numeric_limits<int32_t>::min(), object->get("int32_lower")->asInt32());
	EXPECT_EQ(0u, object->get("int32_lower")->asUInt32());
	EXPECT_EQ(std::numeric_limits<uint32_t>::max(), object->get("uint32_upper")->asUInt32());
	EXPECT_EQ(std::numeric_limits<int64_t>::max(), object->get("int64_upper")->asInt64());
	EXPECT_EQ(static_cast<uint64_t>(int64Limit), object->get("int64_upper")->asUInt64());
	EXPECT_EQ(std::numeric_limits<int64_t>::min(), object->get("int64_lower")->asInt64());
	EXPECT_EQ(0ULL, object->get("int64_lower")->asUInt64());
	EXPECT_EQ(std::numeric_limits<uint64_t>::max(), object->get("uint64_upper")->asUInt64());
	EXPECT_EQ(std::numeric_limits<int64_t>::max(), object->get("uint64_upper")->asInt64());
	EXPECT_EQ(std::numeric_limits<int32_t>::max(), object->get("positive_infinity")->asInt32());
	EXPECT_EQ(std::numeric_limits<uint32_t>::max(), object->get("positive_infinity")->asUInt32());
	EXPECT_EQ(std::numeric_limits<int64_t>::max(), object->get("positive_infinity")->asInt64());
	EXPECT_EQ(std::numeric_limits<uint64_t>::max(), object->get("positive_infinity")->asUInt64());
	EXPECT_EQ(std::numeric_limits<int32_t>::min(), object->get("negative_infinity")->asInt32());
	EXPECT_EQ(0u, object->get("negative_infinity")->asUInt32());
	EXPECT_EQ(std::numeric_limits<int64_t>::min(), object->get("negative_infinity")->asInt64());
	EXPECT_EQ(0ULL, object->get("negative_infinity")->asUInt64());
	EXPECT_EQ(0, object->get("nan")->asInt32());
	EXPECT_EQ(0u, object->get("nan")->asUInt32());
	EXPECT_EQ(0LL, object->get("nan")->asInt64());
	EXPECT_EQ(0ULL, object->get("nan")->asUInt64());

	ASSERT_TRUE(array->append(1e-5));
	ASSERT_TRUE(array->append(1e6));
	ASSERT_TRUE(array->append(12345.875));
	ASSERT_TRUE(array->append(-12345.875));
	EXPECT_EQ(0LL, array->get(0u)->asInt64());
	EXPECT_EQ(0ULL, array->get(0u)->asUInt64());
	EXPECT_EQ(1000000LL, array->get(1u)->asInt64());
	EXPECT_EQ(1000000ULL, array->get(1u)->asUInt64());
	EXPECT_EQ(12345LL, array->get(2u)->asInt64());
	EXPECT_EQ(12345ULL, array->get(2u)->asUInt64());
	EXPECT_EQ(-12345LL, array->get(3u)->asInt64());
	EXPECT_EQ(0ULL, array->get(3u)->asUInt64());

	object->release();
	array->release();
}

TEST(r12_variant_real, formatting_and_parsing_ignore_global_cpp_locale)
{
	ScopedGlobalLocale locale(std::locale(std::locale::classic(), new CommaDecimalPoint()));
	WTSVariant* object = WTSVariant::createObject();
	ASSERT_NE(object, nullptr);
	ASSERT_TRUE(object->append("fraction", 1234.5));
	EXPECT_EQ("1234.5", object->get("fraction")->asString());
	EXPECT_DOUBLE_EQ(1234.5, object->get("fraction")->asDouble());
	EXPECT_EQ(1234, object->get("fraction")->asInt64());
	object->release();
}

TEST(r12_variant_real, formatting_and_parsing_ignore_c_numeric_locale)
{
	ScopedCNumericLocale locale;
#ifdef _WIN32
	const char* candidates[] = {"German_Germany.1252", "French_France.1252", "de-DE", "fr-FR"};
#else
	const char* candidates[] = {
		"de_DE.UTF-8", "de_DE.utf8", "de_DE",
		"fr_FR.UTF-8", "fr_FR.utf8", "fr_FR",
		"it_IT.UTF-8", "it_IT.utf8", "it_IT"
	};
#endif
	bool commaDecimalAvailable = false;
	for (const char* candidate : candidates)
	{
		if (!locale.trySet(candidate))
			continue;
		const std::lconv* numeric = std::localeconv();
		if (numeric != nullptr && numeric->decimal_point != nullptr && numeric->decimal_point[0] == ',')
		{
			commaDecimalAvailable = true;
			break;
		}
	}
	if (!commaDecimalAvailable)
		GTEST_SKIP() << "No installed LC_NUMERIC locale uses a comma decimal separator";

	WTSVariant* object = WTSVariant::createObject();
	ASSERT_NE(object, nullptr);
	ASSERT_TRUE(object->append("fraction", 1234.5));
	ASSERT_TRUE(object->append("million", 1e6));
	ASSERT_TRUE(object->append("tiny", 1e-5));
	EXPECT_EQ("1234.5", object->get("fraction")->asString());
	EXPECT_DOUBLE_EQ(1234.5, object->get("fraction")->asDouble());
	EXPECT_EQ(1234, object->get("fraction")->asInt64());
	EXPECT_EQ(1000000, object->get("million")->asInt64());
	EXPECT_EQ(0, object->get("tiny")->asInt64());
	object->release();
}

TEST(r12_variant_object, missing_keys_return_defaults)
{
	WTSVariant* obj = WTSVariant::createObject();
	obj->append("exist", (int32_t)5);
	EXPECT_TRUE(obj->has("exist"));
	EXPECT_FALSE(obj->has("absent"));
	// 非对象类型（数组）上 has 恒 false
	WTSVariant* arr = WTSVariant::createArray();
	EXPECT_FALSE(arr->has("exist"));
	EXPECT_EQ(arr->get("exist"), nullptr);

	// 缺失键一律回落到显式默认值
	EXPECT_EQ(obj->getInt32("absent", -1), -1);
	EXPECT_EQ(obj->getUInt32("absent", 7u), 7u);
	EXPECT_EQ(obj->getInt64("absent", -8LL), -8LL);
	EXPECT_EQ(obj->getUInt64("absent", 9ULL), 9ULL);
	EXPECT_DOUBLE_EQ(obj->getDouble("absent", 1.25), 1.25);
	EXPECT_EQ(obj->getString("absent", "def"), std::string("def"));
	EXPECT_STREQ(obj->getCString("absent", "d2"), "d2");
	EXPECT_TRUE(obj->getBoolean("absent", true));
	EXPECT_FALSE(obj->getBoolean("absent"));
	// 默认参数本身
	EXPECT_EQ(obj->getInt32("absent"), 0);
	EXPECT_EQ(obj->getString("absent"), std::string(""));
	// std::string 重载与 char* 重载等价
	const std::string key = "exist";
	ASSERT_NE(obj->get(key), nullptr);
	EXPECT_EQ(obj->get(key)->asInt32(), 5);
	EXPECT_EQ(obj->get(std::string("absent")), nullptr);
	obj->release();
	arr->release();
}

TEST(r12_variant_object, overwrite_key_releases_old_value)
{
	WTSVariant* obj = WTSVariant::createObject();
	obj->append("k", (int32_t)1);
	WTSVariant* first = obj->get("k");
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first->retainCount(), 1u);
	EXPECT_EQ(obj->size(), 1u);

	// 同键再挂：旧值被容器释放，新值不带额外引用
	WTSVariant* second = WTSVariant::createObject();
	obj->append("sub", second, true);
	EXPECT_EQ(second->retainCount(), 2u);
	// 覆盖 sub：旧对象引用回到 1，由调用方继续持有
	obj->append("sub", (int32_t)2);
	EXPECT_EQ(second->retainCount(), 1u);
	EXPECT_EQ(obj->size(), 2u);
	EXPECT_EQ(obj->get("sub")->asInt32(), 2);
	second->release();
	// 空指针不覆盖、返回 false
	EXPECT_FALSE(obj->append("sub", (WTSVariant*)NULL));
	EXPECT_EQ(obj->get("sub")->asInt32(), 2);
	obj->release();
}

TEST(r12_variant_object, member_names_and_size_of_children)
{
	WTSVariant* obj = WTSVariant::createObject();
	obj->append("b", (int32_t)2);
	obj->append("a", (int32_t)1);
	obj->append("c", (int32_t)3);
	Names names = obj->memberNames();
	EXPECT_EQ(names.size(), (std::size_t)3);
	EXPECT_TRUE(std::find(names.begin(), names.end(), "a") != names.end());
	EXPECT_TRUE(std::find(names.begin(), names.end(), "z") == names.end());

	// 嵌套对象/数组的 size 语义
	WTSVariant* child = WTSVariant::createObject();
	child->append("inner", (int32_t)9);
	obj->append("child", child, false);
	EXPECT_EQ(child->retainCount(), 1u);
	EXPECT_EQ(obj->get("child")->size(), 1u);
	EXPECT_EQ(obj->get("child")->get("inner")->asInt32(), 9);
	// 标量的 size 恒 0
	EXPECT_EQ(obj->get("a")->size(), 0u);
	// 释放父容器时子容器一并回收（子容器引用计数只有一份，挂在父的 map 上）
	obj->release();
}

TEST(r12_variant_array, append_all_types_and_index_read)
{
	WTSVariant* arr = WTSVariant::createArray();
	ASSERT_NE(arr, nullptr);
	EXPECT_TRUE(arr->isArray());
	EXPECT_FALSE(arr->isObject());
	EXPECT_EQ(arr->size(), 0u);

	EXPECT_TRUE(arr->append((int32_t)1));
	EXPECT_TRUE(arr->append((uint32_t)2));
	EXPECT_TRUE(arr->append((int64_t)3));
	EXPECT_TRUE(arr->append((uint64_t)4));
	EXPECT_TRUE(arr->append(5.5));
	EXPECT_TRUE(arr->append("six"));
	EXPECT_TRUE(arr->append(true));
	WTSVariant* sub = WTSVariant::createObject();
	sub->append("x", (int32_t)7);
	EXPECT_TRUE(arr->append(sub, false));
	EXPECT_EQ(arr->size(), 8u);

	EXPECT_EQ(arr->get(0u)->asInt32(), 1);
	EXPECT_EQ(arr->get(1u)->asUInt32(), 2u);
	EXPECT_EQ(arr->get(2u)->asInt64(), 3);
	EXPECT_EQ(arr->get(3u)->asUInt64(), 4ULL);
	EXPECT_DOUBLE_EQ(arr->get(4u)->asDouble(), 5.5);
	EXPECT_STREQ(arr->get(5u)->asCString(), "six");
	EXPECT_TRUE(arr->get(6u)->asBoolean());
	EXPECT_EQ(arr->get(7u)->type(), WTSVariant::VT_Object);
	EXPECT_EQ(arr->get(7u)->get("x")->asInt32(), 7);
	// 越界下标返回 NULL
	EXPECT_EQ(arr->get(8u), nullptr);
	EXPECT_EQ(arr->get(1000u), nullptr);
	// 对象上按下标读取返回 NULL
	WTSVariant* obj = WTSVariant::createObject();
	EXPECT_EQ(obj->get(0u), nullptr);
	obj->release();
	// 数组上按名字读取返回 NULL
	EXPECT_EQ(arr->get("nokey"), nullptr);
	arr->release();
}

TEST(r12_variant_array, append_wrong_container_returns_false)
{
	WTSVariant* obj = WTSVariant::createObject();
	// 对象容器不接受无名字追加
	EXPECT_FALSE(obj->append((int32_t)1));
	EXPECT_FALSE(obj->append((uint32_t)1));
	EXPECT_FALSE(obj->append((int64_t)1));
	EXPECT_FALSE(obj->append((uint64_t)1));
	EXPECT_FALSE(obj->append(1.0));
	EXPECT_FALSE(obj->append("s"));
	EXPECT_FALSE(obj->append(false));
	EXPECT_FALSE(obj->append(WTSVariant::createObject(), true));
	EXPECT_FALSE(obj->append((WTSVariant*)NULL));
	EXPECT_EQ(obj->size(), 0u);
	obj->release();

	WTSVariant* arr = WTSVariant::createArray();
	// 数组容器不接受带名字追加
	EXPECT_FALSE(arr->append("k", (int32_t)1));
	EXPECT_FALSE(arr->append("k", "v"));
	EXPECT_FALSE(arr->append("k", 1.0));
	EXPECT_FALSE(arr->append("k", true));
	EXPECT_FALSE(arr->append("k", WTSVariant::createObject()));
	EXPECT_FALSE(arr->append("k", (WTSVariant*)NULL));
	EXPECT_EQ(arr->size(), 0u);
	// 数组挂空指针同样被拒绝
	EXPECT_FALSE(arr->append((WTSVariant*)NULL));
	arr->release();
}

TEST(r12_variant_refcount, retain_release_pairing_on_children)
{
	WTSVariant* parent = WTSVariant::createObject();
	WTSVariant* child = WTSVariant::createArray();
	EXPECT_EQ(child->retainCount(), 1u);
	parent->append("arr", child, true);
	EXPECT_EQ(child->retainCount(), 2u);
	// 同一个子对象挂到第二个键上，引用计数继续累加
	parent->append("arr2", child, true);
	EXPECT_EQ(child->retainCount(), 3u);
	// 调用方交回自己那一份
	child->release();
	EXPECT_EQ(child->retainCount(), 2u);
	// 第三个键：父容器现在持有 arr/arr2/arr3 三份引用
	parent->append("arr3", child, true);
	EXPECT_EQ(child->retainCount(), 3u);
	// 释放父容器：三个键各减一次 → 归零并销毁子对象（此后不得再触碰 child）
	parent->release();
	SUCCEED();
}

TEST(r12_variant_conversions, cross_type_numeric_conversion)
{
	WTSVariant* obj = WTSVariant::createObject();
	obj->append("num", "12.75");
	WTSVariant* v = obj->get("num");
	// 字符串按 atof/strtoll/strtod 换算
	EXPECT_DOUBLE_EQ(v->asDouble(), 12.75);
	obj->append("num_prefix", "12.75tail");
	EXPECT_DOUBLE_EQ(obj->get("num_prefix")->asDouble(), 12.75);
	EXPECT_EQ(12LL, obj->get("num_prefix")->asInt64());
	EXPECT_EQ(12ULL, obj->get("num_prefix")->asUInt64());
	EXPECT_EQ(v->asInt32(), 12);
	EXPECT_EQ(v->asUInt32(), 12u);
	EXPECT_EQ(v->asInt64(), 12LL);
	EXPECT_EQ(v->asUInt64(), 12ULL);
	obj->append("big", "9223372036854775807");
	EXPECT_EQ(obj->get("big")->asInt64(), 9223372036854775807LL);
	// 对象/数组类型不匹配时读取全部回落到 0/""
	WTSVariant* nested = WTSVariant::createObject();
	nested->append("leaf", (int32_t)1);
	obj->append("obj", nested, false);
	WTSVariant* ov = obj->get("obj");
	EXPECT_EQ(ov->asInt32(), 0);
	EXPECT_EQ(ov->asUInt32(), 0u);
	EXPECT_EQ(ov->asInt64(), 0LL);
	EXPECT_EQ(ov->asUInt64(), 0ULL);
	EXPECT_DOUBLE_EQ(ov->asDouble(), 0.0);
	EXPECT_EQ(ov->asString(), std::string(""));
	EXPECT_STREQ(ov->asCString(), "");
	obj->release();
}

// 直接对公开创建的 object/array 调 asBoolean，验证不会把容器 union 成员当作 string 解引用。
// VT_Null 只能通过派生类触达受保护的默认构造；其 union 未初始化，验证此路径不读取 union。
TEST(r12_variant_refcount, as_boolean_on_container_and_null_is_safe)
{
	WTSVariant* obj = WTSVariant::createObject();
	WTSVariant* arr = WTSVariant::createArray();
	ASSERT_NE(obj, nullptr);
	ASSERT_NE(arr, nullptr);
	EXPECT_FALSE(obj->asBoolean());
	EXPECT_FALSE(arr->asBoolean());

	class NullVariantForTest : public WTSVariant
	{
	public:
		NullVariantForTest() : WTSVariant() {}
	} nullValue;
	EXPECT_EQ(nullValue.type(), WTSVariant::VT_Null);
	EXPECT_FALSE(nullValue.asBoolean());

	// Scalar controls retain the current string/bool/numeric conversion behavior.
	WTSVariant* scalars = WTSVariant::createObject();
	ASSERT_NE(scalars, nullptr);
	EXPECT_TRUE(scalars->append("bool_true", true));
	EXPECT_TRUE(scalars->append("bool_false", false));
	EXPECT_TRUE(scalars->append("text_yes", "YeS"));
	EXPECT_TRUE(scalars->append("text_no", "NO"));
	EXPECT_TRUE(scalars->append("number", (int32_t)1));
	EXPECT_TRUE(scalars->get("bool_true")->asBoolean());
	EXPECT_FALSE(scalars->get("bool_false")->asBoolean());
	EXPECT_TRUE(scalars->get("text_yes")->asBoolean());
	EXPECT_FALSE(scalars->get("text_no")->asBoolean());
	EXPECT_FALSE(scalars->get("number")->asBoolean());

	scalars->release();
	arr->release();
	obj->release();
}
