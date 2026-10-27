/*! \file shutdown_policies.hpp
* \brief 关闭策略
*
* \details 本文件包含了 thread_pool 的关闭策略。
* 关闭策略控制了线程池在不再被引用时的行为。
* 这些策略决定了如何处理未完成的任务和工作线程的终止。
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


#ifndef THREADPOOL_SHUTDOWN_POLICIES_HPP_INCLUDED
#define THREADPOOL_SHUTDOWN_POLICIES_HPP_INCLUDED



/**
 * @brief threadpool 命名空间包含线程池及相关的工具类
 */
namespace boost { namespace threadpool
{


/**
 * @brief 等待所有任务完成的关闭策略
 *
 * @details 该关闭策略会等待所有任务完成，然后才终止工作线程。
 * 这是最安全的关闭策略，因为它确保所有提交的任务都会被执行。
 *
 * @tparam Pool 线程池的核心类型
 */
  template<typename Pool>
  class wait_for_all_tasks
  {
  public:
    /**
     * @brief 关闭线程池
     * @details 等待所有任务完成，然后终止所有工作线程。该方法会阻塞直到所有任务执行完毕。
     * @param pool 要关闭的线程池引用
     */
    static void shutdown(Pool& pool)
    {
      pool.wait();
      pool.terminate_all_workers(true);
    }
  };


  /**
   * @brief 等待活动任务完成的关闭策略
   *
   * @details 该关闭策略会先清空任务队列，然后等待当前正在执行的任务完成，
   * 最后终止工作线程。与 wait_for_all_tasks 不同的是，它不会等待队列中尚未开始执行的任务。
   *
   * @tparam Pool 线程池的核心类型
   */
  template<typename Pool>
  class wait_for_active_tasks
  {
  public:
    /**
     * @brief 关闭线程池
     * @details 清空任务队列，等待当前活动的任务完成，然后终止所有工作线程。
     * 只有已经开始执行的任务会被等待完成，未执行的任务将被清除。
     * @param pool 要关闭的线程池引用
     */
    static void shutdown(Pool& pool)
    {
      pool.clear();
      pool.wait();
      pool.terminate_all_workers(true);
    }
  };


  /**
   * @brief 立即关闭策略，不等待任务或工作线程终止
   *
   * @details 该关闭策略不等待任何任务完成。它会清空任务队列，并且不等待工作线程终止。
   * 尽管如此，当前正在执行的任务还是会被完全处理完毕。
   * 这是最快速的关闭策略，适用于不需要等待队列中任务完成的场景。
   *
   * @tparam Pool 线程池的核心类型
   */
  template<typename Pool>
  class immediately
  {
  public:
    /**
     * @brief 关闭线程池
     * @details 清空任务队列，然后立即终止所有工作线程，不等待线程终止完成。
     * terminate_all_workers 的 false 参数表示不等待线程终止。
     * @param pool 要关闭的线程池引用
     */
    static void shutdown(Pool& pool)
    {
      pool.clear();
      pool.terminate_all_workers(false);
    }
  };

} } // namespace boost::threadpool

#endif // THREADPOOL_SHUTDOWN_POLICIES_HPP_INCLUDED
