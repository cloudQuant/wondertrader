/*!
 * \file decimal.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 浮点数辅助类,主要用于浮点数据的比较
 */
#pragma once
#include <math.h>

/**
 * @brief 浮点数相关操作的命名空间
 * @details 封装了一系列用于浮点数比较和操作的函数，解决浮点数精度问题
 */
namespace decimal
{
	/**
	 * @brief 浮点数比较的精度常量
	 * @details 用于浮点数比较时的误差容忍度，当两个浮点数的差的绝对值小于此常量时，认为它们相等
	 */
	const double EPSINON = 1e-6;

	/**
	 * @brief 对浮点数进行舍入操作
	 * @param v 需要舍入的浮点数
	 * @param exp 精度系数，默认为1
	 * @return double 舍入后的结果
	 * @details 将浮点数v乘以exp后进行四舍五入，再除以exp，实现对浮点数的精度控制
	 *          例如：rnd(1.2345, 100)将返回1.23，实现保留两位小数
	 */
	inline double rnd(double v, int exp = 1) 
	{
		return round(v*exp) / exp;
	}

	/**
	 * @brief 判断两个浮点数是否相等
	 * @param a 第一个浮点数
	 * @param b 第二个浮点数，默认为0.0
	 * @return bool 如果两个数的差的绝对值小于EPSINON则返回true，否则返回false
	 * @details 由于浮点数精度问题，直接使用==比较可能不准确，此函数通过判断差值是否小于精度常量来判断相等
	 */
	inline bool eq(double a, double b = 0.0) noexcept
	{
		return(fabs(a - b) < EPSINON);
	}

	/**
	 * @brief 判断第一个浮点数是否大于第二个浮点数
	 * @param a 第一个浮点数
	 * @param b 第二个浮点数，默认为0.0
	 * @return bool 如果a确实大于b（考虑精度误差）则返回true，否则返回false
	 * @details 当a-b大于精度常量EPSINON时，认为a确实大于b
	 */
	inline bool gt(double a, double b = 0.0) noexcept
	{
		return a - b > EPSINON;
	}

	/**
	 * @brief 判断第一个浮点数是否小于第二个浮点数
	 * @param a 第一个浮点数
	 * @param b 第二个浮点数，默认为0.0
	 * @return bool 如果a确实小于b（考虑精度误差）则返回true，否则返回false
	 * @details 当b-a大于精度常量EPSINON时，认为a确实小于b
	 */
	inline bool lt(double a, double b = 0.0) noexcept
	{
		return b - a > EPSINON;
	}

	/**
	 * @brief 判断第一个浮点数是否大于等于第二个浮点数
	 * @param a 第一个浮点数
	 * @param b 第二个浮点数，默认为0.0
	 * @return bool 如果a大于或等于b则返回true，否则返回false
	 * @details 结合gt和eq函数，当a大于b或a等于b时，返回true
	 */
	inline bool ge(double a, double b = 0.0) noexcept
	{
		return gt(a, b) || eq(a, b);
	}

	/**
	 * @brief 判断第一个浮点数是否小于等于第二个浮点数
	 * @param a 第一个浮点数
	 * @param b 第二个浮点数，默认为0.0
	 * @return bool 如果a小于或等于b则返回true，否则返回false
	 * @details 结合lt和eq函数，当a小于b或a等于b时，返回true
	 */
	inline bool le(double a, double b = 0.0) noexcept
	{
		return lt(a, b) || eq(a, b);
	}

	/**
	 * @brief 计算浮点数的模运算结果
	 * @param a 被除数
	 * @param b 除数
	 * @return double 模运算的结果
	 * @details 此函数实现了浮点数的模运算，返回值范围在(-0.5, 0.5)之间
	 *          与传统的取模运算不同，它返回的是除法结果的小数部分
	 *          计算方法是a/b的实际值减去a/b四舍五入后的整数值
	 */
	inline double mod(double a, double b)
	{
		return a / b - round(a / b);
	}
	
};