/*! \file future.hpp
* \brief Future 模式的内部实现
*
* \details 本文件实现了 Future 模式的核心组件，包括 future_impl 和 future_impl_task_func 类。
* Future 模式允许异步执行任务并在以后获取结果。这些类为线程池库的高级功能提供了基础。
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


#ifndef THREADPOOL_DETAIL_FUTURE_IMPL_HPP_INCLUDED
#define THREADPOOL_DETAIL_FUTURE_IMPL_HPP_INCLUDED


#include "locking_ptr.hpp"

#include <boost/smart_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/utility/result_of.hpp>
#include <boost/static_assert.hpp>
#include <boost/type_traits.hpp>

/**
 * @brief boost::threadpool::detail 命名空间包含了线程池库的内部实现细节
 * @details 该命名空间中的类和函数为线程池库的公共接口提供支持，通常不应直接使用
 */
namespace boost { namespace threadpool { namespace detail 
{

/**
 * @brief Future 模式的内部实现类
 * @details 该类提供了异步执行的核心功能，包括结果存储、状态检查、等待和通知机制。
 * 它允许在未来某个时间点检索返回值，无需立即获取结果。
 * @tparam Result 异步任务的返回类型
 */
template<class Result> 
class future_impl
{
public:
  /**
   * @brief 任务函数的结果类型
   * @details 指定了函数对象返回值的常量引用类型
   */
  typedef Result const & result_type;

  /**
   * @brief Future 对象的结果类型
   * @details 指定了 Future 对象存储的结果类型
   */
  typedef Result future_result_type;
  
  /**
   * @brief Future 实现类的类型
   * @details 定义了 future_impl 类的类型别名，便于内部引用
   */
  typedef future_impl<future_result_type> future_type;

private:
    /**
     * @brief 结果就绪状态标志
     * @details 标记任务是否已完成并且结果可用
     */
    volatile bool m_ready;
    
    /**
     * @brief 存储任务执行的结果
     * @details 当任务完成时，结果将存储在这个变量中
     */
    volatile future_result_type m_result;

    /**
     * @brief 互斥锁，用于保护对共享数据的访问
     * @details 用于同步访问共享状态和结果
     */
    mutable mutex m_monitor;
    
    /**
     * @brief 条件变量，用于等待结果准备就绪
     * @details 当结果就绪时通知等待的线程
     */
    mutable condition m_condition_ready;

    /**
     * @brief 取消状态标志
     * @details 标记任务是否已被取消
     */
    volatile bool m_is_cancelled;
    
    /**
     * @brief 执行状态标志
     * @details 标记任务是否正在执行中
     */
    volatile bool m_executing;

public:


public:

  /**
   * @brief 构造函数
   * @details 创建一个新的 future_impl 对象，初始化其状态为未就绪、未取消
   */
  future_impl()
  : m_ready(false)
  , m_is_cancelled(false)
  , m_executing(false)
  {
  }

  /**
   * @brief 检查结果是否就绪
   * @details 判断任务是否已经完成并且结果已经可用
   * @return 如果结果就绪返回 true，否则返回 false
   */
  bool ready() const volatile
  {
    return m_ready; 
  }

  /**
   * @brief 等待结果可用
   * @details 阻塞当前线程直到结果可用或任务已取消
   *          通过条件变量实现等待机制
   */
  void wait() const volatile
  {
    const future_type* self = const_cast<const future_type*>(this);
    mutex::scoped_lock lock(self->m_monitor);

    while(!m_ready)
    {
      self->m_condition_ready.wait(lock);
    }
  }


  /**
   * @brief 带超时的结果等待
   * @details 阻塞当前线程直到结果可用、任务已取消或超时
   * @param timestamp 超时的时间点，使用 boost::xtime 表示
   * @return 如果在超时前结果就绪返回 true，如果超时返回 false
   */
  bool timed_wait(boost::xtime const & timestamp) const
  {
    const future_type* self = const_cast<const future_type*>(this);
    mutex::scoped_lock lock(self->m_monitor);

    while(!m_ready)
    {
      if(!self->m_condition_ready.timed_wait(lock, timestamp)) return false;
    }

    return true;
  }


  /**
   * @brief 函数调用操作符，获取任务结果
   * @details 等待任务完成并返回结果。如果结果尚未就绪，将阻塞直到结果可用
   * @return 返回任务执行的结果
   */
  result_type operator()() const volatile
  {
    wait();
/*
    if( throw_exception_ != 0 )
    {
      throw_exception_( this );
    }
*/
 
    return *(const_cast<const future_result_type*>(&m_result));
  }


  /**
   * @brief 设置任务的执行结果
   * @details 当任务完成时，调用此方法将结果设置到 Future 对象中，
   *          并通知所有等待的线程。只有在任务未就绪且未取消时才会设置
   * @param r 要设置的任务结果
   */
  void set_value(future_result_type const & r) volatile
  {
    locking_ptr<future_type, mutex> lockedThis(*this, m_monitor);
    if(!m_ready && !m_is_cancelled)
    {
      lockedThis->m_result = r;
      lockedThis->m_ready = true;
      lockedThis->m_condition_ready.notify_all();
    }
  }
/*
  template<class E> void set_exception() // throw()
  {
    m_impl->template set_exception<E>();
  }

  template<class E> void set_exception( char const * what ) // throw()
  {
    m_impl->template set_exception<E>( what );
  }
  */


   /**
    * @brief 取消任务
    * @details 尝试取消未完成或正在执行中的任务。
    *          如果任务已完成，则无法取消
    * @return 如果取消成功返回 true，如果任务已完成无法取消则返回 false
    */
   bool cancel() volatile
   {
     if(!m_ready || m_executing)
     {
        m_is_cancelled = true;
        return true;
     }
     else
     {
       return false;
     }
   }


   /**
    * @brief 检查任务是否已取消
    * @details 返回当前任务的取消状态
    * @return 如果任务已被取消返回 true，否则返回 false
    */
   bool is_cancelled() const volatile
   {
     return m_is_cancelled;
   }


   /**
    * @brief 设置任务的执行状态
    * @details 更新任务的执行状态标志，标记任务是否正在执行中
    * @param executing 如果任务正在执行设置为 true，否则设置为 false
    */
   void set_execution_status(bool executing) volatile
   {
     m_executing = executing;
   }
};


/**
 * @brief Future 任务函数对象的实现
 * @details 封装一个函数对象并将其结果存储到 Future 对象中。
 *          这个类作为线程池与 Future 机制之间的桥梁
 * @tparam Future Future 类型模板
 * @tparam Function 函数对象类型
 */
template<
  template <typename> class Future,
  typename Function
>
class future_impl_task_func
{

public:
  /**
   * @brief 函数对象的结果类型
   * @details 指示当前函数对象的返回值类型为 void
   */
  typedef void result_type;

  /**
   * @brief 原始函数类型
   * @details 指示被封装的原始函数对象的类型
   */
  typedef Function function_type;
  
  /**
   * @brief Future 的结果类型
   * @details 指示原始函数执行后返回结果的类型
   */
  typedef typename result_of<function_type()>::type future_result_type;
  
  /**
   * @brief Future 对象类型
   * @details 根据结果类型生成的 Future 实例类型
   */
  typedef Future<future_result_type> future_type;

  /**
   * 静态断言：确保任务函数是无参数函数
   */
  BOOST_STATIC_ASSERT(function_traits<function_type()>::arity == 0);

  /**
   * 静态断言：确保任务函数的返回类型不是 void
   */
  BOOST_STATIC_ASSERT(!is_void<future_result_type>::value);

private:
  /**
   * @brief 原始函数对象
   * @details 被封装的可执行函数对象，其结果将被存储到 Future 中
   */
  function_type             m_function;
  
  /**
   * @brief Future 对象的共享指针
   * @details 用于存储任务结果的 Future 对象，使用智能指针管理其生命周期
   */
  shared_ptr<future_type>   m_future;

public:
  /**
   * @brief 构造函数
   * @details 创建一个 future_impl_task_func 对象并初始化其函数和 Future 对象
   * @param function 要执行的函数对象
   * @param future 存储结果的 Future 对象
   */
  future_impl_task_func(function_type const & function, shared_ptr<future_type> const & future)
  : m_function(function)
  , m_future(future)
  {
  }

  /**
   * @brief 函数调用操作符
   * @details 执行被封装的函数并将结果存储到 Future 对象中。
   *          如果任务已被取消，则不会执行函数并存储其结果
   */
  void operator()()
  {
    if(m_function)
    {
      m_future->set_execution_status(true);
      if(!m_future->is_cancelled())
      {
        // TODO future exeception handling 
        m_future->set_value(m_function());
      }
      m_future->set_execution_status(false); // TODO consider exceptions
    }
  }

};





} } } // namespace boost::threadpool::detail

#endif // THREADPOOL_DETAIL_FUTURE_IMPL_HPP_INCLUDED


