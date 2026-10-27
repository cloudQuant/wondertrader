/*! \file threadpool.hpp
* \brief 线程池库主要包含文件
*
* \details 使用完整的线程池库时，只需要包含此文件即可。
* 该文件统一导入了线程池库的所有必要组件，简化了使用过程。
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

#ifndef THREADPOOL_HPP_INCLUDED
#define THREADPOOL_HPP_INCLUDED

/**
 * @brief 线程池库组件包含
 */

/**
 * @brief 包含异步计算结果的 future 类实现
 * @details 提供了获取异步任务执行结果的机制
 */
#include "./threadpool/future.hpp"

/**
 * @brief 线程池核心类实现
 * @details 实现了线程池的创建、管理和任务调度机制
 */
#include "./threadpool/pool.hpp"

/**
 * @brief 线程池适配器
 * @details 包含对线程池的各种适配扩展，提供不同的执行策略
 */
#include "./threadpool/pool_adaptors.hpp"

/**
 * @brief 任务适配器
 * @details 将不同类型的可调用对象适配为线程池能够处理的任务格式
 */
#include "./threadpool/task_adaptors.hpp"


#endif // THREADPOOL_HPP_INCLUDED

