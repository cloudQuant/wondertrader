/*! \file size_policies.hpp
* \brief 大小策略
*
* \details 本文件包含了 thread_pool 的大小策略。
* 大小策略用于控制线程池中工作线程的数量。
* 这些策略决定了线程池如何动态或静态地调整其大小。
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


#ifndef THREADPOOL_SIZE_POLICIES_HPP_INCLUDED
#define THREADPOOL_SIZE_POLICIES_HPP_INCLUDED



/**
 * @brief threadpool 命名空间包含线程池及相关的工具类
 */
namespace boost { namespace threadpool
{

  /**
   * @brief 不提供任何功能的大小策略控制器
   *
   * @details 这是一个空的控制器实现，仅定义了构造函数但不提供实际的功能。
   * 当不需要对线程池大小进行控制时使用。
   *
   * @tparam Pool 线程池的核心类型
   */
  template<typename Pool>
  struct empty_controller
  {
    /**
     * @brief 构造函数
     * @param 线程池的大小策略（未使用）
     * @param 线程池的共享指针（未使用）
     */
    empty_controller(typename Pool::size_policy_type&, shared_ptr<Pool>) {}
  };


  /**
   * @brief 允许调整大小的大小策略控制器
   *
   * @details 该控制器实现允许动态地调整线程池中的工作线程数量。
   * 可以通过该控制器增加或减少线程池中的线程数量。
   *
   * @tparam Pool 线程池的核心类型
   */
  template< typename Pool >
  class resize_controller
  {
    /**
     * @brief 大小策略类型的别名
     */
    typedef typename Pool::size_policy_type size_policy_type;

    /**
     * @brief 对大小策略的引用包装器
     * @details 存储对大小策略对象的引用，使控制器可以操作它
     */
    reference_wrapper<size_policy_type> m_policy;

    /**
     * @brief 对线程池的共享指针
     * @details 确保只要控制器存在，线程池对象就不会被释放，从而保证策略指针的有效性
     */
    shared_ptr<Pool> m_pool;

  public:
    /**
     * @brief 构造函数
     * @details 创建一个大小调整控制器，它引用线程池的大小策略并保持对线程池的引用
     * @param policy 要控制的大小策略对象
     * @param pool 与该策略相关联的线程池对象
     */
    resize_controller(size_policy_type& policy, shared_ptr<Pool> pool)
      : m_policy(policy)
      , m_pool(pool)
    {
    }

    /**
     * @brief 调整线程池大小
     * @details 通过调用内部大小策略的 resize 方法，改变线程池中的线程数量
     * @param worker_count 期望的工作线程数量
     * @return 如果调整成功返回 true，否则返回 false
     */
    bool resize(size_t worker_count)
    {
      return m_policy.get().resize(worker_count);
    }
  };


  /**
   * @brief 保持线程数量的大小策略
   *
   * @details 该策略维持线程池中固定数量的工作线程。如果工作线程意外死亡，
   * 它会自动补充新的线程以维持原有的线程数量。
   * 这种策略适合于需要固定并可预测的线程资源的场景。
   *
   * @tparam Pool 线程池的核心类型
   */
  template<typename Pool>
  class static_size
  {
    /**
     * @brief 对线程池的易变引用包装器
     * @details 存储对线程池对象的引用，使大小策略可以操作线程池
     * 这里使用 volatile 修饰是因为线程池可能在多线程环境中被修改
     */
    reference_wrapper<Pool volatile> m_pool;

  public:
    /**
     * @brief 初始化线程池大小
     * @details 设置线程池的初始大小，即工作线程的数量
     * @param pool 要初始化的线程池对象
     * @param worker_count 期望的工作线程数量
     */
    static void init(Pool& pool, size_t const worker_count)
    {
      pool.resize(worker_count);
    }

    /**
     * @brief 构造函数
     * @details 创建一个静态大小策略对象，它引用线程池对象
     * @param pool 要控制的线程池对象
     */
    static_size(Pool volatile & pool)
      : m_pool(pool)
    {}

    /**
     * @brief 调整线程池大小
     * @details 改变线程池中的工作线程数量
     * @param worker_count 期望的工作线程数量
     * @return 如果调整成功返回 true，否则返回 false
     */
    bool resize(size_t const worker_count)
    {
      return m_pool.get().resize(worker_count);
    }

    /**
     * @brief 处理工作线程意外死亡的情况
     * @details 当工作线程意外终止时，自动创建一个新的工作线程以维持原有的线程数量
     * @param new_worker_count 当前的工作线程数量（在线程死亡后）
     */
    void worker_died_unexpectedly(size_t const new_worker_count)
    {
      m_pool.get().resize(new_worker_count + 1);
    }

    /**
     * @brief 任务调度通知方法
     * @details 当有新任务调度时调用。目前该方法未实现。
     * @note TODO: 这些函数目前还未被调用
     */
    void task_scheduled() {}

    /**
     * @brief 任务完成通知方法
     * @details 当任务执行完成时调用。目前该方法未实现。
     * @note TODO: 这些函数目前还未被调用
     */
    void task_finished() {}
  };

} } // namespace boost::threadpool

#endif // THREADPOOL_SIZE_POLICIES_HPP_INCLUDED
