/*! \file pool_adaptors.hpp
* \brief 线程池适配器
*
* \details 本文件包含了线程池的易用适配器，类似于智能指针的使用方式。
* 提供了多个便捷的 schedule 函数重载，简化了任务调度过程。
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


#ifndef THREADPOOL_POOL_ADAPTORS_HPP_INCLUDED
#define THREADPOOL_POOL_ADAPTORS_HPP_INCLUDED

#include <boost/smart_ptr.hpp>


namespace boost { namespace threadpool
{


    /**
     * @brief 将一个 Runnable 对象调度到线程池中异步执行
     * 
     * @details Runnable 是一个拥有 run() 成员函数的任意类。这是 pool->schedule(bind(&Runnable::run, task_object)) 的便捷写法。
     * 通过这个函数，可以直接将具有 run() 成员函数的对象提交到线程池中。
     * 
     * @tparam Pool 线程池类型
     * @tparam Runnable 可运行对象类型，需要拥有 run() 成员函数
     * @param pool 线程池实例
     * @param obj 可运行对象，其 run() 成员函数将被执行，该函数不应该抛出异常
     * @return 如果任务成功调度则返回 true，否则返回 false
     */  
    template<typename Pool, typename Runnable>
    bool schedule(Pool& pool, shared_ptr<Runnable> const & obj)
    {	
      return pool->schedule(bind(&Runnable::run, obj));
    }	
    
    /**
     * @brief 将任务函数对象提交到线程池中异步执行
     * 
     * @details 任务将被执行一次。这个函数重载直接接受池实例引用。
     * 注意这个版本只适用于返回类型为 void 的任务函数。
     * 
     * @tparam Pool 线程池类型
     * @param pool 线程池实例引用
     * @param task 要执行的任务函数对象
     * @return 如果任务成功调度则返回 true，否则返回 false
     */  
    template<typename Pool>
    typename enable_if < 
      is_void< typename result_of< typename Pool::task_type() >::type >,
      bool
    >::type
    schedule(Pool& pool, typename Pool::task_type const & task)
    {	
      return pool.schedule(task);
    }	


    /**
     * @brief 将任务函数对象提交到线程池中异步执行（智能指针版本）
     * 
     * @details 任务将被执行一次。这个函数重载接受指向池实例的智能指针。
     * 便于在使用智能指针管理线程池的场景下使用。
     * 注意这个版本只适用于返回类型为 void 的任务函数。
     * 
     * @tparam Pool 线程池类型
     * @param pool 指向线程池实例的智能指针
     * @param task 要执行的任务函数对象
     * @return 如果任务成功调度则返回 true，否则返回 false
     */
    template<typename Pool>
    typename enable_if < 
      is_void< typename result_of< typename Pool::task_type() >::type >,
      bool
    >::type
    schedule(shared_ptr<Pool> const pool, typename Pool::task_type const & task)
    {	
      return pool->schedule(task);
    }	


} } // namespace boost::threadpool

#endif // THREADPOOL_POOL_ADAPTORS_HPP_INCLUDED


