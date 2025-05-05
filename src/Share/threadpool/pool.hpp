/*! \file pool.hpp
* \brief 线程池核心实现
*
* \details 本文件包含线程池的核心类：pool<Task, SchedulingPolicy>。
*
* 线程池是一种在同一进程内实现异步和并行处理的机制。
* pool 类提供了一种便捷的方式，将函数对象作为异步任务调度执行。
* 通过使用自定义的调度器，可以轻松控制这些任务的调度方式。
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


#ifndef THREADPOOL_POOL_HPP_INCLUDED
#define THREADPOOL_POOL_HPP_INCLUDED

#include <boost/ref.hpp>

#include "./detail/pool_core.hpp"

#include "task_adaptors.hpp"

#include "./detail/locking_ptr.hpp"

#include "scheduling_policies.hpp"
#include "size_policies.hpp"
#include "shutdown_policies.hpp"



/// @brief threadpool 命名空间包含线程池及相关工具类
/// @details 提供了线程池实现及各种调度策略、大小策略和关闭策略
/// @see thread_pool, fifo_pool, lifo_pool, prio_pool
namespace boost { namespace threadpool
{



  /** 
   * @brief 线程池类
   * 
   * @details 线程池是一种在同一进程内实现异步和并行处理的机制。
   * pool 类提供了一种便捷的方式，将函数对象作为异步任务调度执行。
   * 任务的调度方式可以通过使用自定义的调度器轻松控制。
   * 任务函数不应该抛出异常。
   * 
   * 线程池类支持默认构造、拷贝构造和赋值。
   * 它具有引用语义，同一个池的所有副本都是等价的且可以互换的。
   * 除了赋值操作外，池上的所有操作都是完全线程安全或顺序一致的；
   * 也就是说，并发调用的行为就好像这些调用是按照未指定的顺序顺序发出的。
   * 
   * @tparam Task 任务函数对象类型，实现了 'void operator() (void) const' 操作符。
   * operator() 由池调用来执行任务。任何抛出的异常都会被忽略。
   * @tparam SchedulingPolicy 任务容器类型，决定了任务如何调度。
   * 保证每次只有一个线程访问这个容器。调度器不应抛出异常。
   * @tparam SizePolicy 大小策略，用于控制线程池中线程的数量
   * @tparam SizePolicyController 大小策略控制器，用于调整线程池的大小
   * @tparam ShutdownPolicy 关闭策略，决定线程池关闭时的行为
   * 
   * @note 线程池类是线程安全的。
   * 
   * @see 任务类型： task_func, prio_task_func
   * @see 调度策略： fifo_scheduler, lifo_scheduler, prio_scheduler
   * @see 大小策略： static_size, dynamic_size
   * @see 关闭策略： wait_for_all_tasks, immediately
   */ 
  template <
    typename Task                                   = task_func,
    template <typename> class SchedulingPolicy      = fifo_scheduler,
    template <typename> class SizePolicy            = static_size,
    template <typename> class SizePolicyController  = resize_controller,
    template <typename> class ShutdownPolicy        = wait_for_all_tasks
  > 
  class thread_pool 
  {
    /**
     * @brief 线程池内部核心类型
     * @details 定义了线程池内部实现所使用的核心类型，包含了所有模板参数
     */
    typedef detail::pool_core<Task, 
                              SchedulingPolicy,
                              SizePolicy,
                              SizePolicyController,
                              ShutdownPolicy> pool_core_type;
    
    /**
     * @brief 指向核心实现的智能指针
     * @details 使用 pimpl 模式将实现细节隐藏在内部类中
     */
    shared_ptr<pool_core_type>          m_core;
    
    /**
     * @brief 关闭控制器
     * @details 当持有指向核心的最后一个池被删除时，控制器将关闭池
     */
    shared_ptr<void>                    m_shutdown_controller;

  public: // Type definitions
    /**
     * @brief 任务类型
     * @details 指示线程池中执行的任务函数对象类型
     */
    typedef Task task_type;
    
    /**
     * @brief 调度器类型
     * @details 指示用于任务调度的容器类型
     */
    typedef SchedulingPolicy<task_type> scheduler_type;
    
    /*
    typedef thread_pool<Task, 
                        SchedulingPolicy,
                        SizePolicy,
                        ShutdownPolicy > pool_type;          //!< Indicates the thread pool's type.
    */
    
    /**
     * @brief 大小策略类型
     * @details 指示用于控制线程池大小的策略类型
     */
    typedef SizePolicy<pool_core_type> size_policy_type;
    
    /**
     * @brief 大小控制器类型
     * @details 指示用于操作线程池大小的控制器类型
     */
    typedef SizePolicyController<pool_core_type> size_controller_type;


  public:
    /**
     * @brief 构造函数
     * 
     * @details 创建一个新的线程池实例。建立核心实现并设置关闭控制器。
     * 线程池创建时会立即调整大小以匹配指定的线程数量。
     * 
     * @param initial_threads 初始线程数量。池立即调整大小以设置指定的线程数量。
     * 池的实际线程数量取决于 SizePolicy 策略。默认值为 0。
     */
    thread_pool(size_t initial_threads = 0)
    : m_core(new pool_core_type)
    , m_shutdown_controller(static_cast<void*>(0), bind(&pool_core_type::shutdown, m_core))
    {
      size_policy_type::init(*m_core, initial_threads);
    }


    /**
     * @brief 获取管理线程池中线程数量的大小控制器
     * 
     * @details 返回一个大小控制器对象，可用于检查和调整线程池的大小。
     * 通过这个对象可以动态地控制线程池中的线程数量。
     * 
     * @return 大小控制器对象
     * @see SizePolicy 大小策略类
     */
    size_controller_type size_controller()
    {
      return m_core->size_controller();
    }


    /**
     * @brief 获取线程池中的线程数量
     * 
     * @details 返回当前线程池中活动线程的数量。这个值可能会根据线程池的大小策略而动态变化。
     * 
     * @return 线程池中的线程数量
     */
    size_t size()	const
    {
      return m_core->size();
    }


     /**
      * @brief 将任务调度用于异步执行
      * 
      * @details 将任务函数对象添加到线程池的调度队列中，并在线程可用时异步执行。
      * 每个任务只会执行一次。如果线程池正在关闭或已经关闭，则任务可能不会被执行。
      * 
      * @param task 任务函数对象。它不应该抛出异常，因为任何异常都将被忽略。
      * @return 如果任务能够被调度则返回 true，否则返回 false。
      */  
     bool schedule(task_type const & task)
     {	
       return m_core->schedule(task);
     }


    /**
     * @brief 返回当前正在执行的任务数量
     * 
     * @details 获取线程池中当前正在执行的活动任务数量。
     * 这个值表示有多少任务正在线程池的线程中运行。
     * 
     * @return 活动任务的数量
     */  
    size_t active() const
    {
      return m_core->active();
    }


    /**
     * @brief 返回准备执行的任务数量
     * 
     * @details 获取线程池中当前已调度但尚未开始执行的任务数量。
     * 这个值表示调度器中有多少任务正在等待执行。
     * 
     * @return 等待任务的数量
     */  
    size_t pending() const
    {
      return m_core->pending();
    }


    /**
     * @brief 移除线程池调度器中的所有等待任务
     * 
     * @details 清除线程池的调度器中所有待处理的任务。
     * 这些任务将不再执行。正在执行的任务不受影响。
     */  
    void clear()
    { 
      m_core->clear();
    }    


    /**
     * @brief 检查是否没有等待任务
     * 
     * @details 判断线程池的调度器中是否没有待处理的任务。
     * 这个函数比使用 'pending() == 0' 检查更高效。
     * 
     * @return 如果没有准备执行的任务则返回 true
     * @note 这个函数比检查 'pending() == 0' 更高效
     */   
    bool empty() const
    {
      return m_core->empty();
    }	


    /**
     * @brief 阻塞当前线程直到任务总数小于等于指定阈值
     * 
     * @details 当前线程的执行被阻塞，直到所有活动和等待任务的总和等于或小于给定的阈值。
     * 这个方法允许程序等待直到线程池中的工作负载降低到指定水平。
     * 
     * @param task_threshold 线程池和调度器中任务的最大数量阈值。默认值为 0，
     * 表示等待直到所有任务完成。
     */     
    void wait(size_t task_threshold = 0) const
    {
      m_core->wait(task_threshold);
    }	


    /**
     * @brief 阻塞当前线程直到指定时间或任务数量满足条件
     * 
     * @details 当前线程的执行被阻塞，直到达到指定的时间点或者所有活动和等待任务的总和
     * 等于或小于给定的阈值。这个方法在等待任务完成的同时提供了超时机制。
     * 
     * @param timestamp 函数最迟返回的时间点
     * @param task_threshold 线程池和调度器中任务的最大数量阈值。默认值为 0，
     * 表示等待直到所有任务完成。
     * @return 如果任务总数等于或小于阈值返回 true，否则（超时情况下）返回 false
     */       
    bool wait(xtime const & timestamp, size_t task_threshold = 0) const
    {
      return m_core->wait(timestamp, task_threshold);
    }
  };



  /**
   * @brief FIFO（先进先出）线程池
   *
   * @details 按照先进先出的原则调度任务的线程池。
   * 最早提交的任务将最先执行。使用 task_func 函数对象类型作为任务，
   * static_size 策略控制线程数量，并在关闭时等待所有任务完成。
   */ 
  typedef thread_pool<task_func, fifo_scheduler, static_size, resize_controller, wait_for_all_tasks> fifo_pool;


  /**
   * @brief LIFO（后进先出）线程池
   *
   * @details 按照后进先出的原则调度任务的线程池。
   * 最新提交的任务将最先执行。这种模式适用于可能依赖于新近提交任务的场景。
   * 使用 task_func 函数对象类型作为任务。
   */ 
  typedef thread_pool<task_func, lifo_scheduler, static_size, resize_controller, wait_for_all_tasks> lifo_pool;


  /**
   * @brief 优先级任务线程池
   *
   * @details 按照任务优先级调度任务的线程池。
   * 使用 prio_task_func 函数对象类型作为任务，允许基于任务的优先级进行调度。
   * 高优先级的任务将先于低优先级的任务执行。
   */ 
  typedef thread_pool<prio_task_func, prio_scheduler, static_size, resize_controller, wait_for_all_tasks> prio_pool;


  /**
   * @brief 标准线程池
   *
   * @details 这是一个标准的线程池类型，等同于 fifo_pool。
   * 按照先进先出的原则调度 task_func 函数对象。
   * 这是最常用的线程池类型，适合大多数应用场景。
   */ 
  typedef fifo_pool pool;



} } // namespace boost::threadpool

#endif // THREADPOOL_POOL_HPP_INCLUDED
