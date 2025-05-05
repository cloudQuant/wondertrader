/*! \file task_adaptors.hpp
* \brief 任务适配器
*
* \details 本文件包含了任务函数对象的适配器。
* 这些适配器封装了不同类型的任务，如标准任务、带优先级的任务和循环任务，
* 使它们可以在线程池中执行。
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


#ifndef THREADPOOL_TASK_ADAPTERS_HPP_INCLUDED
#define THREADPOOL_TASK_ADAPTERS_HPP_INCLUDED


#include <boost/smart_ptr.hpp>
#include <boost/function.hpp>
#include <boost/thread.hpp>
#include <time.h>

/**
 * @brief threadpool 命名空间包含线程池及相关的工具类
 */
namespace boost { namespace threadpool
{

  /**
   * @brief 标准任务函数对象
   *
   * @details 此函数对象封装了一个无参数且返回类型为 void 的函数。
   * 通过调用操作符 () 来执行被封装的函数。
   * 这是线程池中最基本的任务类型。
   *
   * @see boost function 库
   */
  typedef function0<void> task_func;




  /**
   * @brief 带优先级的任务函数对象
   *
   * @details 此函数对象封装了一个 task_func 对象并为其绑定了优先级。
   * prio_task_func 可以使用操作符 < 进行比较，实现了任务的偏序排序。
   * 被封装的任务函数通过调用操作符 () 来执行。
   * 这种任务类型允许基于优先级的任务调度。
   *
   * @see prio_scheduler 优先级调度器
   */
  class prio_task_func
  {
  private:
    /**
     * @brief 任务函数的优先级
     * @details 数值越大表示优先级越高
     */
    unsigned int m_priority;
    
    /**
     * @brief 任务的函数对象
     * @details 被封装的实际任务函数
     */
    task_func m_function;

  public:
    typedef void result_type; //!< Indicates the functor's result type.

  public:
    /**
     * @brief 构造函数
     * @details 创建一个带优先级的任务函数对象
     * @param priority 任务的优先级，数值越大表示优先级越高
     * @param function 任务的函数对象
     */
    prio_task_func(unsigned int const priority, task_func const & function)
      : m_priority(priority)
      , m_function(function)
    {
    }

    /**
     * @brief 执行任务函数
     * @details 如果任务函数有效，则调用它。这个方法会被线程池调用来执行任务。
     */
    void operator() (void) const
    {
      if(m_function)
      {
        m_function();
      }
    }

    /**
     * @brief 比较运算符，实现基于优先级的偏序排序
     * @details 此运算符使得优先级任务可以通过优先级大小进行比较和排序。
     * 在优先级队列中使用时，将使高优先级的任务先被处理。
     * @param rhs 要比较的对象
     * @return 如果当前对象的优先级小于参数对象的优先级，返回 true，否则返回 false
     */
    bool operator< (const prio_task_func& rhs) const
    {
      return m_priority < rhs.m_priority; 
    }

  };  // prio_task_func



 




  /**
   * @brief 循环任务函数对象
   *
   * @details 此函数对象封装了一个返回布尔值的线程函数对象。
   * 被封装的任务函数通过调用操作符 () 来执行，并且它会在定期的时间间隔内执行，
   * 直到返回 false 为止。间隔长度可以为零。
   * 请注意，只要任务处于循环状态，线程池的线程就会一直被占用。
   * 这种任务类型适用于需要定期执行的操作，如定时检查或监控。
   */
  class looped_task_func
  {
  private:
    /**
     * @brief 任务的函数对象
     * @details 返回布尔值的函数，定期执行直到返回 false
     */
    function0<bool> m_function;
    
    /**
     * @brief 休息时间的秒数部分
     * @details 每次任务执行间的休息时间（秒）
     */
    unsigned int m_break_s;
    
    /**
     * @brief 休息时间的纳秒部分
     * @details 每次任务执行间的休息时间（纳秒）
     */
    unsigned int m_break_ns;

  public:
    typedef void result_type; //!< Indicates the functor's result type.

  public:
    /**
     * @brief 构造函数
     * @details 创建一个循环任务函数对象，它会定期执行函数直到函数返回 false
     * @param function 要循环执行的任务函数对象，当返回 false 时循环结束
     * @param interval 任务函数首次执行前和后续执行之间的最小休息时间（毫秒），默认为 0
     */
    looped_task_func(function0<bool> const & function, unsigned int const interval = 0)
      : m_function(function)
    {
      m_break_s  = interval / 1000;
      m_break_ns = (interval - m_break_s * 1000) * 1000 * 1000;
    }

    /*! Executes the task function.
    */
    void operator() (void) const
    {
      if(m_function)
      {
        if(m_break_s > 0 || m_break_ns > 0)
        { // Sleep some time before first execution
          xtime xt;
          xtime_get(&xt, boost::TIME_UTC_);
          xt.nsec += m_break_ns;
          xt.sec += m_break_s;
          thread::sleep(xt); 
        }

        while(m_function())
        {
          if(m_break_s > 0 || m_break_ns > 0)
          {
            xtime xt;
            xtime_get(&xt, boost::TIME_UTC_);
            xt.nsec += m_break_ns;
            xt.sec += m_break_s;
            thread::sleep(xt); 
          }
          else
          {
            thread::yield(); // Be fair to other threads
          }
        }
      }
    }

  }; // looped_task_func


} } // namespace boost::threadpool

#endif // THREADPOOL_TASK_ADAPTERS_HPP_INCLUDED

