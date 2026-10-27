/*! \file pool_core.hpp
* \brief 线程池核心实现
*
* \details 本文件包含线程池的核心类：pool_core<Task, SchedulingPolicy, SizePolicy, SizePolicyController, ShutdownPolicy>。
*
* 线程池是一种在同一进程内进行异步和并行处理的机制。
* pool_core 类提供了一种方便的方式来调度异步任务（以函数对象的形式）。
* 这些任务的调度可以通过使用自定义的调度器轻松控制。
* 该类是线程池实现的基础，提供了任务管理、线程管理和调度策略。
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


#ifndef THREADPOOL_POOL_CORE_HPP_INCLUDED
#define THREADPOOL_POOL_CORE_HPP_INCLUDED




#include "locking_ptr.hpp"
#include "worker_thread.hpp"

#include "../task_adaptors.hpp"

#include <boost/thread.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/bind.hpp>
#include <boost/static_assert.hpp>
#include <boost/type_traits.hpp>

#include <vector>


/**
 * @brief threadpool 命名空间包含了线程池及相关的实用工具类
 * @details detail 子命名空间包含线程池库的内部实现细节，这些组件
 *          通常不由用户直接使用，而是由高级接口调用
 */
namespace boost { namespace threadpool { namespace detail 
{

  /**
   * @brief 线程池核心实现类
   * 
   * @details 线程池是一种在同一进程内进行异步和并行处理的机制。
   * pool_core 类提供了一种方便的方式来调度异步任务（以函数对象的形式）。
   * 这些任务的调度可以通过使用自定义的调度器轻松控制。
   * 任务执行时不应抛出异常。
   *
   * pool_core 类可以默认构造，但不可复制。
   *
   * @tparam Task 任务函数对象类型，实现了 'void operator() (void) const' 操作符。
   *               该操作符由线程池调用来执行任务。异常将被忽略。
   * @tparam SchedulingPolicy 任务调度策略，决定任务如何调度。
   *                          保证同一时间只有一个线程访问该容器。调度器不应抛出异常。
   * @tparam SizePolicy 线程数量策略，控制线程池中线程的数量
   * @tparam SizePolicyController 线程数量控制器，管理线程池大小的调整
   * @tparam ShutdownPolicy 关闭策略，定义线程池关闭时的行为
   * 
   * @note 线程池类是线程安全的。
   * 
   * @see 任务类型: task_func, prio_task_func
   * @see 调度策略: fifo_scheduler, lifo_scheduler, prio_scheduler
   */
  template <
    typename Task, 

    template <typename> class SchedulingPolicy,
    template <typename> class SizePolicy,
    template <typename> class SizePolicyController,
    template <typename> class ShutdownPolicy
  > 
  class pool_core
  : public enable_shared_from_this< pool_core<Task, SchedulingPolicy, SizePolicy, SizePolicyController, ShutdownPolicy > > 
  , private noncopyable
  {

  public: // Type definitions
    /**
     * @brief 任务类型
     * @details 指定线程池处理的任务函数对象类型
     */
    typedef Task task_type;                                 //!< Indicates the task's type.
    /**
     * @brief 调度器类型
     * @details 指定用于任务调度的策略类型
     */
    typedef SchedulingPolicy<task_type> scheduler_type;     //!< Indicates the scheduler's type.
    /**
     * @brief 线程池类型
     * @details 指定当前线程池类型，用于内部类型定义和引用
     */
    typedef pool_core<Task, 
                      SchedulingPolicy, 
                      SizePolicy,
                      SizePolicyController,
                      ShutdownPolicy > pool_type;           //!< Indicates the thread pool's type.
    /**
     * @brief 大小策略类型
     * @details 指定控制线程池大小的策略类型
     */
    typedef SizePolicy<pool_type> size_policy_type;         //!< Indicates the sizer's type.
    /**
     * @brief 大小控制器类型
     * @details 指定用于操作线程池大小的控制器类型
     */
    typedef SizePolicyController<pool_type> size_controller_type;
    /**
     * @brief 关闭策略类型
     * @details 指定线程池关闭时的行为策略
     */
    typedef ShutdownPolicy<pool_type> shutdown_policy_type;//!< Indicates the shutdown policy's type.  
    /**
     * @brief 工作线程类型
     * @details 指定线程池中工作线程的类型
     */
    typedef worker_thread<pool_type> worker_type;

    /**
     * @brief 静态断言：确保任务是无参数函数
     * @details 检查任务函数类型的参数数量必须为 0，即无参数函数
     */
    BOOST_STATIC_ASSERT(function_traits<task_type()>::arity == 0);

    /**
     * @brief 静态断言：确保任务函数的返回类型为 void
     * @details 检查任务函数的返回值类型必须为 void，因为线程池不处理返回值
     */
    BOOST_STATIC_ASSERT(is_void<typename result_of<task_type()>::type >::value);


  private:  // Friends 
    friend class worker_thread<pool_type>;

#if defined(__SUNPRO_CC) && (__SUNPRO_CC <= 0x580)  // Tested with CC: Sun C++ 5.8 Patch 121018-08 2006/12/06
   friend class SizePolicy;
   friend class ShutdownPolicy;
#else
   friend class SizePolicy<pool_type>;
   friend class ShutdownPolicy<pool_type>;
#endif

  private: // The following members may be accessed by _multiple_ threads at the same time:
    /**
     * @brief 当前工作线程数量
     * @details 记录线程池中当前工作线程的总数，可被多个线程同时访问
     */
    volatile size_t m_worker_count;
    
    /**
     * @brief 目标工作线程数量
     * @details 线程池要维持的目标线程数量，用于动态调整线程池大小
     */
    volatile size_t m_target_worker_count;
    
    /**
     * @brief 活动的工作线程数量
     * @details 当前正在执行任务的工作线程数量
     */
    volatile size_t m_active_worker_count;
      


  private: // The following members are accessed only by _one_ thread at the same time:
    /**
     * @brief 任务调度器
     * @details 负责任务的存储和调度，根据策略决定任务执行的顺序
     */
    scheduler_type  m_scheduler;
    
    /**
     * @brief 线程池大小策略
     * @details 控制线程池大小策略的智能指针，保证不为空，负责线程的自动扩容和缩小
     */
    scoped_ptr<size_policy_type> m_size_policy;
    
    /**
     * @brief 终止所有工作线程标志
     * @details 指示是否触发了所有工作线程的终止操作
     */
    bool  m_terminate_all_workers;
    
    /**
     * @brief 已终止的工作线程列表
     * @details 存储已经终止但尚未完全析构的工作线程
     */
    std::vector<shared_ptr<worker_type> > m_terminated_workers;
    
  private: // The following members are implemented thread-safe:
    /**
     * @brief 递归互斥锁
     * @details 用于保护方法访问共享数据，支持同一线程多次锁定
     */
    mutable recursive_mutex  m_monitor;
    
    /**
     * @brief 工作线程空闲或终止事件
     * @details 当工作线程变为空闲状态或被终止时触发的条件变量
     */
    mutable condition m_worker_idle_or_terminated_event;
    
    /**
     * @brief 任务可用或终止线程事件
     * @details 当有新任务可用或需要减少工作线程总数时触发的条件变量
     */
    mutable condition m_task_or_terminate_workers_event;

  public:
    /**
     * @brief 构造函数
     * @details 初始化线程池核心对象，设置线程计数器和终止标志
     *          初始状态下线程池不包含任何工作线程，需要调用resize方法增加线程
     */
    pool_core()
      : m_worker_count(0) 
      , m_target_worker_count(0)
      , m_active_worker_count(0)
      , m_terminate_all_workers(false)
    {
      pool_type volatile & self_ref = *this;
      m_size_policy.reset(new size_policy_type(self_ref));

      m_scheduler.clear();
    }


    /**
     * @brief 析构函数
     * @details 释放线程池相关资源
     *          注意：根据 RAII 原则，智能指针成员和容器会自动清理
     */
    ~pool_core()
    {
    }

    /**
     * @brief 获取线程池大小控制器
     * @details 返回用于管理线程池中线程数量的大小控制器对象
     *          通过该控制器可以动态调整线程池大小
     * @return 大小控制器对象
     * @see SizePolicy
     */
    size_controller_type size_controller()
    {
      return size_controller_type(*m_size_policy, this->shared_from_this());
    }

    /**
     * @brief 获取线程池中线程的数量
     * @details 返回当前线程池中存在的工作线程总数
     *          这包括空闲和正在执行任务的线程
     * @return 线程数量
     */
    size_t size()	const volatile
    {
      return m_worker_count;
    }

    /**
     * @brief 关闭线程池
     * @details 使用指定的关闭策略关闭线程池
     *          根据关闭策略不同，可能等待所有任务完成、只等待活动任务或者立即关闭
     * @note 该方法只会被调用一次
     */
    void shutdown()
    {
      ShutdownPolicy<pool_type>::shutdown(*this);
    }

    /**
     * @brief 调度任务进行异步执行
     * @details 将任务添加到线程池的任务队列中进行异步执行
     *          任务将仅执行一次。调度后，通知一个空闲的工作线程来执行此任务
     * @param task 要执行的任务函数对象，不应抛出异常
     * @return 如果任务成功调度返回 true，否则返回 false
     */  
    bool schedule(task_type const & task) volatile
    {	
      locking_ptr<pool_type, recursive_mutex> lockedThis(*this, m_monitor); 
      
      if(lockedThis->m_scheduler.push(task))
      {
        lockedThis->m_task_or_terminate_workers_event.notify_one();
        return true;
      }
      else
      {
        return false;
      }
    }	


    /**
     * @brief 返回当前正在执行的任务数量
     * @details 获取当前线程池中正在执行任务的工作线程数量
     *          此值表示线程池的当前负载情况
     * @return 活动任务数量
     */  
    size_t active() const volatile
    {
      return m_active_worker_count;
    }


    /**
     * @brief 返回待执行的任务数量
     * @details 获取已添加到调度器中但尚未开始执行的任务数量
     *          这些任务正在等待可用的工作线程来执行它们
     * @return 待执行的任务数量
     */  
    size_t pending() const volatile
    {
      locking_ptr<const pool_type, recursive_mutex> lockedThis(*this, m_monitor);
      return lockedThis->m_scheduler.size();
    }


    /**
     * @brief 清除所有待处理的任务
     * @details 从线程池的调度器中移除所有尚未开始执行的任务
     *          注意：已经开始执行的任务不会被中断
     */  
    void clear() volatile
    { 
      locking_ptr<pool_type, recursive_mutex> lockedThis(*this, m_monitor);
      lockedThis->m_scheduler.clear();
    }    


    /**
     * @brief 检查是否没有待处理的任务
     * @details 判断调度器中是否没有任务等待执行
     *          这个函数比检查 'pending() == 0' 更高效，因为它不需要获取实际数量
     * @return 如果没有任务等待执行则返回 true
     */   
    bool empty() const volatile
    {
      locking_ptr<const pool_type, recursive_mutex> lockedThis(*this, m_monitor);
      return lockedThis->m_scheduler.empty();
    }	


    /**
     * @brief 等待任务数量降低到指定阈值
     * @details 阻塞当前执行线程，直到所有活动任务和待处理任务的总和小于或等于给定的阈值
     *          当阈值为 0 时，表示等待所有任务完成
     * @param task_threshold 任务数量阈值，默认为 0（等待所有任务完成）
     */     
    void wait(size_t const task_threshold = 0) const volatile
    {
      const pool_type* self = const_cast<const pool_type*>(this);
      recursive_mutex::scoped_lock lock(self->m_monitor);

      if(0 == task_threshold)
      {
        while(0 != self->m_active_worker_count || !self->m_scheduler.empty())
        { 
          self->m_worker_idle_or_terminated_event.wait(lock);
        }
      }
      else
      {
        while(task_threshold < self->m_active_worker_count + self->m_scheduler.size())
        { 
          self->m_worker_idle_or_terminated_event.wait(lock);
        }
      }
    }	

    /**
     * @brief 带超时的任务等待
     * @details 阻塞当前执行线程，直到达到指定的时间戳或所有活动任务和待处理任务的总和
     *          小于或等于给定的阈值。该方法提供了超时机制，避免无限期等待
     * @param timestamp 最迟返回的时间戳
     * @param task_threshold 任务数量阈值，默认为 0（等待所有任务完成）
     * @return 如果任务总和小于或等于阈值返回 true，如果超时返回 false
     */       
    bool wait(xtime const & timestamp, size_t const task_threshold = 0) const volatile
    {
      const pool_type* self = const_cast<const pool_type*>(this);
      recursive_mutex::scoped_lock lock(self->m_monitor);

      if(0 == task_threshold)
      {
        while(0 != self->m_active_worker_count || !self->m_scheduler.empty())
        { 
          if(!self->m_worker_idle_or_terminated_event.timed_wait(lock, timestamp)) return false;
        }
      }
      else
      {
        while(task_threshold < self->m_active_worker_count + self->m_scheduler.size())
        { 
          if(!self->m_worker_idle_or_terminated_event.timed_wait(lock, timestamp)) return false;
        }
      }

      return true;
    }


  private:	

    /**
     * @brief 终止所有工作线程
     * @details 触发所有工作线程的终止过程。将目标线程数量设置为 0，
     *          并发送信号通知所有工作线程考虑终止
     * @param wait 是否等待所有线程终止完成
     *              当为 true 时，方法会阻塞直到所有工作线程终止
     *              当为 false 时，只发送终止信号然后立即返回
     */
    void terminate_all_workers(bool const wait) volatile
    {
      pool_type* self = const_cast<pool_type*>(this);
      recursive_mutex::scoped_lock lock(self->m_monitor);

      self->m_terminate_all_workers = true;

      m_target_worker_count = 0;
      self->m_task_or_terminate_workers_event.notify_all();

      if(wait)
      {
        while(m_active_worker_count > 0)
        {
          self->m_worker_idle_or_terminated_event.wait(lock);
        }

        for(typename std::vector<shared_ptr<worker_type> >::iterator it = self->m_terminated_workers.begin();
          it != self->m_terminated_workers.end();
          ++it)
        {
          (*it)->join();
        }
        self->m_terminated_workers.clear();
      }
    }


    /**
     * @brief 改变线程池中工作线程的数量
     * @details 根据指定的目标线程数量调整线程池大小
     *          如果目标线程数量大于当前数量，则创建新线程
     *          如果目标线程数量小于当前数量，则通知工作线程终止
     * @param worker_count 新的工作线程数量
     * @return 如果线程池将被调整大小返回 true，否则返回 false
     */
    bool resize(size_t const worker_count) volatile
    {
      locking_ptr<pool_type, recursive_mutex> lockedThis(*this, m_monitor); 

      if(!m_terminate_all_workers)
      {
        m_target_worker_count = worker_count;
      }
      else
      { 
        return false;
      }


      if(m_worker_count <= m_target_worker_count)
      { // increase worker count
        while(m_worker_count < m_target_worker_count)
        {
          try
          {
            worker_thread<pool_type>::create_and_attach(lockedThis->shared_from_this());
            m_worker_count++;
            m_active_worker_count++;	
          }
          catch(thread_resource_error)
          {
            return false;
          }
        }
      }
      else
      { // decrease worker count
        lockedThis->m_task_or_terminate_workers_event.notify_all();   // TODO: Optimize number of notified workers
      }

      return true;
    }


    /**
     * @brief 处理工作线程意外终止的情况
     * @details 当工作线程由于未处理的异常而意外终止时调用此方法
     *          更新工作线程计数并通知等待线程
     *          根据线程池状态决定是将线程添加到终止列表还是通知大小策略
     * @param worker 意外终止的工作线程指针
     */
    void worker_died_unexpectedly(shared_ptr<worker_type> worker) volatile
    {
      locking_ptr<pool_type, recursive_mutex> lockedThis(*this, m_monitor);

      m_worker_count--;
      m_active_worker_count--;
      lockedThis->m_worker_idle_or_terminated_event.notify_all();	

      if(m_terminate_all_workers)
      {
        lockedThis->m_terminated_workers.push_back(worker);
      }
      else
      {
        lockedThis->m_size_policy->worker_died_unexpectedly(m_worker_count);
      }
    }

    /**
     * @brief 处理工作线程正常析构
     * @details 当工作线程正常终止并被析构时调用此方法
     *          更新工作线程计数并通知等待线程
     *          如果线程池处于终止状态，将线程添加到终止列表中
     * @param worker 被析构的工作线程指针
     */
    void worker_destructed(shared_ptr<worker_type> worker) volatile
    {
      locking_ptr<pool_type, recursive_mutex> lockedThis(*this, m_monitor);
      m_worker_count--;
      m_active_worker_count--;
      lockedThis->m_worker_idle_or_terminated_event.notify_all();	

      if(m_terminate_all_workers)
      {
        lockedThis->m_terminated_workers.push_back(worker);
      }
    }


    /**
     * @brief 执行任务
     * @details 从调度器中获取一个任务并执行它
     *          该方法会根据当前线程池状态决定是执行任务还是终止工作线程
     *          当没有任务时，工作线程会进入等待状态，直到有新的任务或需要终止
     * @return 如果工作线程应该继续工作返回 true，如果工作线程应该终止返回 false
     */
    bool execute_task() volatile
    {
      function0<void> task;

      { // fetch task
        pool_type* lockedThis = const_cast<pool_type*>(this);
        recursive_mutex::scoped_lock lock(lockedThis->m_monitor);

        // decrease number of threads if necessary
        if(m_worker_count > m_target_worker_count)
        {	
          return false;	// terminate worker
        }


        // wait for tasks
        while(lockedThis->m_scheduler.empty())
        {	
          // decrease number of workers if necessary
          if(m_worker_count > m_target_worker_count)
          {	
            return false;	// terminate worker
          }
          else
          {
            m_active_worker_count--;
            lockedThis->m_worker_idle_or_terminated_event.notify_all();	
            lockedThis->m_task_or_terminate_workers_event.wait(lock);
            m_active_worker_count++;
          }
        }

        task = lockedThis->m_scheduler.top();
        lockedThis->m_scheduler.pop();
      }

      // call task function
      if(task)
      {
        task();
      }
 
      //guard->disable();
      return true;
    }
  };




} } } // namespace boost::threadpool::detail

#endif // THREADPOOL_POOL_CORE_HPP_INCLUDED
