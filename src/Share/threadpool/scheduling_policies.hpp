/*! \file scheduling_policies.hpp
* \brief 任务调度策略
*
* \details 本文件包含了线程池类的几种基本调度策略。
* 调度策略通过任务容器实现，该容器控制对任务的访问。
* 容器从根本上决定了线程池处理任务的顺序。
* 
* 任务容器不需要是线程安全的，因为它们会被线程池以线程安全的方式使用。
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


#ifndef THREADPOOL_SCHEDULING_POLICIES_HPP_INCLUDED
#define THREADPOOL_SCHEDULING_POLICIES_HPP_INCLUDED


#include <queue>
#include <deque>

#include "task_adaptors.hpp"

/**
 * @brief threadpool 命名空间包含线程池及相关工具类
 */
namespace boost { namespace threadpool
{

  /**
   * @brief 实现FIFO（先进先出）排序的调度策略
   * 
   * @details 该容器实现了FIFO（先进先出）调度策略。
   * 首先添加到调度器的任务将首先被移除。
   * 处理过程按照相同的顺序顺序进行。
   * FIFO表示“先进先出”。
   * 
   * @tparam Task 实现operator()(void)的函数对象
   */ 
  template <typename Task = task_func>  
  class fifo_scheduler
  {
  public:
    /**
     * @brief 调度器的任务类型
     * @details 指示调度器使用的任务函数对象类型
     */
    typedef Task task_type;

  protected:
    /**
     * @brief 内部任务容器
     * @details 使用双端队列存储任务，实现FIFO调度策略
     */
    std::deque<task_type> m_container;


  public:
    /**
     * @brief 向调度器添加新任务
     * @details 将任务添加到队列尾部，符合FIFO（先进先出）策略
     * @param task 任务对象
     * @return 如果任务成功调度返回 true，否则返回 false
     */
    bool push(task_type const & task)
    {
      m_container.push_back(task);
      return true;
    }

    /*! Removes the task which should be executed next.
    */
    void pop()
    {
      m_container.pop_front();
    }

    /*! Gets the task which should be executed next.
    *  \return The task object to be executed.
    */
    task_type const & top() const
    {
      return m_container.front();
    }

    /*! Gets the current number of tasks in the scheduler.
    *  \return The number of tasks.
    *  \remarks Prefer empty() to size() == 0 to check if the scheduler is empty.
    */
    size_t size() const
    {
      return m_container.size();
    }

    /*! Checks if the scheduler is empty.
    *  \return true if the scheduler contains no tasks, false otherwise.
    *  \remarks Is more efficient than size() == 0. 
    */
    bool empty() const
    {
      return m_container.empty();
    }

    /**
     * @brief 移除调度器中的所有任务
     * @details 清空队列中的所有任务
     */  
    void clear()
    {   
      m_container.clear();
    } 
  };



  /**
   * @brief 实现LIFO（后进先出）排序的调度策略
   * 
   * @details 该容器实现了LIFO（后进先出）调度策略。
   * 最近添加到调度器的任务将第一个被移除。
   * LIFO表示“后进先出”，与堆栈结构类似。
   * 
   * @tparam Task 实现operator()(void)的函数对象
   */ 
  template <typename Task = task_func>  
  class lifo_scheduler
  {
  public:
    /**
     * @brief 调度器的任务类型
     * @details 指示调度器使用的任务函数对象类型
     */
    typedef Task task_type;

  protected:
    /**
     * @brief 内部任务容器
     * @details 使用双端队列存储任务，实现LIFO调度策略，新任务添加到队列的头部
     */
    std::deque<task_type> m_container;

  public:
    /**
     * @brief 向调度器添加新任务
     * @details 将任务添加到队列头部，符合LIFO（后进先出）策略
     * @param task 任务对象
     * @return 如果任务成功调度返回 true，否则返回 false
     */
    bool push(task_type const & task)
    {
      m_container.push_front(task);
      return true;
    }

    /*! Removes the task which should be executed next.
    */
    void pop()
    {
      m_container.pop_front();
    }

    /*! Gets the task which should be executed next.
    *  \return The task object to be executed.
    */
    task_type const & top() const
    {
      return m_container.front();
    }

    /*! Gets the current number of tasks in the scheduler.
    *  \return The number of tasks.
    *  \remarks Prefer empty() to size() == 0 to check if the scheduler is empty.
    */
    size_t size() const
    {
      return m_container.size();
    }

    /*! Checks if the scheduler is empty.
    *  \return true if the scheduler contains no tasks, false otherwise.
    *  \remarks Is more efficient than size() == 0. 
    */
    bool empty() const
    {
      return m_container.empty();
    }

    /*! Removes all tasks from the scheduler.
    */  
    void clear()
    {    
      m_container.clear();
    } 

  };



  /**
   * @brief 实现优先级排序的调度策略
   * 
   * @details 该容器实现了基于任务优先级的调度策略。
   * 具有最高优先级的任务将首先被移除并执行。
   * 必须能够使用 operator< 比较两个任务。
   * 
   * @tparam Task 实现 operator() 和 operator< 的函数对象。operator< 必须是偏序关系。
   * 
   * @see prio_thread_func
   */
  template <typename Task = prio_task_func>  
  class prio_scheduler
  {
  public:
    /**
     * @brief 调度器的任务类型
     * @details 指示调度器使用的任务函数对象类型，必须支持优先级比较
     */
    typedef Task task_type;

  protected:
    /**
     * @brief 内部任务容器
     * @details 使用标准库优先级队列存储任务，自动按优先级排序
     */
    std::priority_queue<task_type> m_container;


  public:
    /**
     * @brief 向调度器添加新任务
     * @details 根据任务的优先级将其添加到优先级队列中
     * @param task 任务对象，必须实现了优先级比较操作
     * @return 如果任务成功调度返回 true，否则返回 false
     */
    bool push(task_type const & task)
    {
      m_container.push(task);
      return true;
    }

    /**
     * @brief 移除下一个应该执行的任务
     * @details 移除优先级队列中的最高优先级任务
     */
    void pop()
    {
      m_container.pop();
    }

    /*! Gets the task which should be executed next.
    *  \return The task object to be executed.
    */
    task_type const & top() const
    {
      return m_container.top();
    }

    /*! Gets the current number of tasks in the scheduler.
    *  \return The number of tasks.
    *  \remarks Prefer empty() to size() == 0 to check if the scheduler is empty.
    */
    size_t size() const
    {
      return m_container.size();
    }

    /*! Checks if the scheduler is empty.
    *  \return true if the scheduler contains no tasks, false otherwise.
    *  \remarks Is more efficient than size() == 0. 
    */
    bool empty() const
    {
      return m_container.empty();
    }

    /*! Removes all tasks from the scheduler.
    */  
    void clear()
    {    
      while(!m_container.empty())
      {
        m_container.pop();
      }
    } 
  };


} } // namespace boost::threadpool


#endif // THREADPOOL_SCHEDULING_POLICIES_HPP_INCLUDED

