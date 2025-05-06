/**
 * @file
 * @brief 线程池工作线程
 *
 * 工作线程实例会附加到线程池中，并负责执行线程池中的任务。
 * 每个工作线程都是一个独立的执行单元，可以并发地处理任务，
 * 从而实现高效的多任务处理。
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

#ifndef THREADPOOL_DETAIL_WORKER_THREAD_HPP_INCLUDED
#define THREADPOOL_DETAIL_WORKER_THREAD_HPP_INCLUDED


#include "scope_guard.hpp"

#include <boost/smart_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>


namespace boost { namespace threadpool { namespace detail 
{

  /**
   * @brief 线程池工作线程
   * @details worker_thread 类表示一个执行线程。工作线程被附加到线程池并处理该池中的任务。
   *          工作线程及其内部 boost::thread 的生命周期由系统自动管理。
   *          
   *          此类是一个辅助类，不能直接构造或访问。
   * 
   * @see pool_core
   */ 
  template <typename Pool>
  class worker_thread
  : public enable_shared_from_this< worker_thread<Pool> > 
  , private noncopyable
  {
  public:
    /**
     * @brief 指定所属线程池的类型
     * @details 用于表示该工作线程关联的线程池类型
     */
    typedef Pool pool_type;

  private:
    /**
     * @brief 指向创建该工作线程的线程池的指针
     * @details 此智能指针保持对线程池的引用，确保工作线程可以访问线程池属性和方法
     */
    shared_ptr<pool_type>      m_pool;     
    
    /**
     * @brief 执行运行循环的线程指针
     * @details 指向实际执行 run 方法的 boost::thread 对象
     */
    shared_ptr<boost::thread>  m_thread;

    
    /**
     * @brief 构造一个新的工作线程
     * @details 初始化工作线程并将其关联到指定的线程池
     *          构造函数是私有的，防止直接创建工作线程实例
     * @param pool 指向其父线程池的指针
     * @see function create_and_attach
     */
    worker_thread(shared_ptr<pool_type> const & pool)
    : m_pool(pool)
    {
      assert(pool);
    }

	
	/**
	 * @brief 通知运行循环中发生了异常
	 * @details 当工作线程在执行过程中发生未处理的异常时，通知线程池
	 *          此方法用于异常处理机制，确保线程池可以追踪工作线程的意外终止
	 */
	void died_unexpectedly()
	{
		m_pool->worker_died_unexpectedly(this->shared_from_this());
	}


  public:
	  /**
	   * @brief 按顺序执行线程池的任务
	   * @details 这是工作线程的主执行函数，它在一个循环中不断从线程池获取并执行任务
	   *          实现了异常安全机制：如果发生异常，会通过 scope_guard 通知线程池
	   *          当没有更多任务要执行时，线程会结束并通知线程池已被析构
	   */
	  void run()
	  { 
		  scope_guard notify_exception(bind(&worker_thread::died_unexpectedly, this));

		  while(m_pool->execute_task()) {}

		  notify_exception.disable();
		  m_pool->worker_destructed(this->shared_from_this());
	  }


	  /**
	   * @brief 等待工作线程结束
	   * @details 阻塞调用线程，直到该工作线程执行完成
	   *          这在关闭线程池时等待所有工作线程安全终止时非常有用
	   */
	  void join()
	  {
		  m_thread->join();
	  }


	  /**
	   * @brief 构造新的工作线程并将其附加到线程池
	   * @details 这是创建工作线程的方法，实现了工厂模式
	   *          提供静态方法创建工作线程，并自动启动线程执行 run 方法
	   *          使用智能指针来管理工作线程的生命周期
	   * @param pool 指向线程池的指针
	   */
	  static void create_and_attach(shared_ptr<pool_type> const & pool)
	  {
		  shared_ptr<worker_thread> worker(new worker_thread(pool));
		  if(worker)
		  {
			  worker->m_thread.reset(new boost::thread(bind(&worker_thread::run, worker)));
		  }
	  }

  };


} } } // namespace boost::threadpool::detail

#endif // THREADPOOL_DETAIL_WORKER_THREAD_HPP_INCLUDED

