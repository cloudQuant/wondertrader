/*! \file future.hpp
* \brief 线程池库的 Future 模式实现
*
* \details 本文件实现了 Future 模式，用于异步操作的结果获取。
* Future 允许程序在提交任务到线程池后立即返回，同时可以在任务完成后获取其结果。
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

#ifndef THREADPOOL_FUTURE_HPP_INCLUDED
#define THREADPOOL_FUTURE_HPP_INCLUDED


  
#include "./detail/future.hpp"
#include <boost/utility/enable_if.hpp>

//#include "pool.hpp"
//#include <boost/utility.hpp>

//#include <boost/thread/mutex.hpp>


namespace boost { namespace threadpool
{

  /**
   * @brief Future 模式的实现类模板
   * 
   * @details Future 类提供了异步操作的结果获取机制。它允许程序提交任务到线程池后立即返回，
   * 并且可以在任务完成时获取其返回值。支持等待、超时等待和取消操作。
   * 
   * @tparam Result 异步操作的结果类型
   * 
   * @see schedule 函数，用于向线程池提交任务并返回 future 对象
   */ 


template<class Result> 
class future
{
private:
  /**
   * @brief Future 实现的内部实现对象
   * @details 使用智能指针管理内部实现对象，实现引用计数和自动内存管理
   */
  shared_ptr<detail::future_impl<Result> > m_impl;

public:
    /**
     * @brief 函数结果类型
     * @details 指示异步函数的返回值类型，以常量引用方式返回
     */
    typedef Result const & result_type;
    
    /**
     * @brief Future 结果类型
     * @details 指示 future 对象的结果类型
     */
    typedef Result future_result_type;


public:

  /**
   * @brief 默认构造函数
   * @details 创建一个空的 future 对象，包含一个新的内部实现
   */
  future()
  : m_impl(new detail::future_impl<future_result_type>()) 
  {
  }

  /**
   * @brief 内部使用的构造函数
   * @details 从已存在的内部实现创建 future 对象，仅供内部使用
   * @param impl 内部实现对象的智能指针
   */
  // only for internal usage
  future(shared_ptr<detail::future_impl<Result> > const & impl)
  : m_impl(impl)
  {
  }

  /**
   * @brief 检查任务是否已完成
   * @details 检查异步任务是否已经完成并且结果可用
   * @return 如果结果已经准备好则返回 true，否则返回 false
   */
  bool ready() const
  {
    return m_impl->ready();
  }

  /**
   * @brief 等待任务完成
   * @details 阻塞当前线程直到异步任务完成并且结果可用
   */
  void wait() const
  {
    m_impl->wait();
  }

  /**
   * @brief 带超时的等待任务完成
   * @details 阻塞当前线程直到异步任务完成或者超过指定的时间
   * @param timestamp 超时时间点
   * @return 如果在超时前任务完成则返回 true，如果超时则返回 false
   */
  bool timed_wait(boost::xtime const & timestamp) const
  {
    return m_impl->timed_wait(timestamp);
  }

   /**
    * @brief 运算符重载，获取任务结果
    * @details 等待异步任务完成并返回其结果。如果任务被取消，可能会抛出异常
    * @return 异步任务的结果
    * @throw thread::cancelation_exception 如果任务被取消可能会抛出异常
    */
   result_type operator()() // throw( thread::cancelation_exception, ... )
   {
     return (*m_impl)();
   }

   /**
    * @brief 获取任务结果
    * @details 等待异步任务完成并返回其结果。与 operator() 功能相同
    * @return 异步任务的结果
    * @throw thread::cancelation_exception 如果任务被取消可能会抛出异常
    */
   result_type get() // throw( thread::cancelation_exception, ... )
   {
     return (*m_impl)();
   }

   /**
    * @brief 取消任务
    * @details 尝试取消正在运行的异步任务。如果任务已经完成，则无法取消
    * @return 如果成功取消任务返回 true，如果任务已完成或无法取消返回 false
    */
   bool cancel()
   {
     return m_impl->cancel();
   }

   /**
    * @brief 检查任务是否已被取消
    * @details 返回任务的取消状态
    * @return 如果任务已被取消返回 true，否则返回 false
    */
   bool is_cancelled() const
   {
     return m_impl->is_cancelled();
   }
};





/**
 * @brief 将任务提交到线程池并返回 future 对象
 * @details 将任务提交到指定的线程池中异步执行，并返回一个 future 对象，
 * 该对象可用于等待任务完成和获取任务结果。注意该函数只支持有返回值的任务。
 * @tparam Pool 线程池类型
 * @tparam Function 函数或函数对象类型
 * @param pool 线程池实例
 * @param task 要执行的任务（函数或函数对象）
 * @return 一个 future 对象，表示异步执行的任务
 */
template<class Pool, class Function>
typename disable_if < 
  is_void< typename result_of< Function() >::type >,
  future< typename result_of< Function() >::type >
>::type
schedule(Pool& pool, const Function& task)
{
  typedef typename result_of< Function() >::type future_result_type;

  // create future impl and future
  shared_ptr<detail::future_impl<future_result_type> > impl(new detail::future_impl<future_result_type>);
  future <future_result_type> res(impl);

  // schedule future impl
  pool.schedule(detail::future_impl_task_func<detail::future_impl, Function>(task, impl));

  // return future
  return res;

/*
 TODO
  if(pool->schedule(bind(&Future::run, future)))
  {
    return future;
  }
  else
  {
    // construct empty future
    return error_future;
  }
  */
}



} } // namespace boost::threadpool

#endif // THREADPOOL_FUTURE_HPP_INCLUDED

