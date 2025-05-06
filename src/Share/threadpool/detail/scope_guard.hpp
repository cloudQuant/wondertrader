/**
 * @file
 * @brief 作用域守卫对象实现
 *
 * 此文件实现了作用域守卫（Scope Guard）模式，该模式利用 C++ 的析构函数特性，
 * 确保在特定作用域结束时自动执行特定代码。这在需要进行资源清理或其他
 * 类似「结束处理」操作时非常有用，可以确保代码无论以何种方式退出作用域都能执行必要的清理操作。
 *
 * Copyright (c) 2005-2007 Philipp Henkel
 *
 * Use, modification, and distribution are subject to the
 * Boost Software License, Version 1.0. (See accompanying file
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 * http://threadpool.sourceforge.net
 *
 */


#ifndef THREADPOOL_DETAIL_SCOPE_GUARD_HPP_INCLUDED
#define THREADPOOL_DETAIL_SCOPE_GUARD_HPP_INCLUDED



#include <boost/function.hpp>


namespace boost { namespace threadpool { namespace detail 
{

/**
 * @brief 作用域守卫类
 * @details 实现了RAII（Resource Acquisition Is Initialization）模式，
 *          当对象退出作用域时，自动执行指定的函数。
 *          这种模式特别适合用于确保资源清理、锁的释放等。
 *          继承自 boost::noncopyable 以防止复制，因为复制可能导致清理操作被执行多次。
 */
class scope_guard
: private boost::noncopyable
{
	/**
	 * @brief 退出作用域时要执行的函数
	 * @details 存储传入构造函数的回调函数，该函数将在对象析构时自动执行
	 *          定义为 const 以防止在对象生命周期内被意外修改
	 */
	function0<void> const m_function;

	/**
	 * @brief 守卫对象是否有效的标志
	 * @details 当设置为 false 时，即使对象退出作用域，也不会执行存储的函数
	 *          这允许在某些情况下禁用退出时的清理操作
	 */
	bool                  m_is_active;

public:
	/**
	 * @brief 构造函数
	 * @details 创建一个作用域守卫对象，初始化要在作用域结束时执行的函数
	 *          并将守卫对象设置为激活状态
	 * @param call_on_exit 要在作用域结束时执行的函数对象
	 */
	scope_guard(function0<void> const & call_on_exit)
	: m_function(call_on_exit)
	, m_is_active(true)
	{
	}

	/**
	 * @brief 析构函数
	 * @details 在对象退出作用域时自动调用
	 *          如果守卫对象处于激活状态且存储的函数有效，则执行该函数
	 */
	~scope_guard()
	{
		if(m_is_active && m_function)
		{
			m_function();
		}
	}

	/**
	 * @brief 禁用守卫对象
	 * @details 将守卫对象设置为非激活状态，这样在退出作用域时将不会执行存储的函数
	 *          当需要有条件地跳过清理操作时非常有用
	 */
	void disable()
	{
		m_is_active = false;
	}
};






} } } // namespace boost::threadpool::detail

#endif // THREADPOOL_DETAIL_SCOPE_GUARD_HPP_INCLUDED


