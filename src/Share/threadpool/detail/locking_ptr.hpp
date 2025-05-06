/*! \file locking_ptr.hpp
* \brief 带有作用域锁定机制的智能指针
*
* \details 该类是对易变指针（volatile pointer）的包装器。
* 它通过锁定传入的互斥量来实现对内部指针的同步访问。
* locking_ptr 基于 Andrei Alexandrescu 的 LockingPtr 设计。详细信息可参考
* A. Alexandrescu 的文章 "volatile - Multithreaded Programmer's Best Friend"。
* 
* 这种智能指针在多线程环境下非常有用，可以确保线程安全地访问共享资源。
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


#ifndef THREADPOOL_DETAIL_LOCKING_PTR_HPP_INCLUDED
#define THREADPOOL_DETAIL_LOCKING_PTR_HPP_INCLUDED

#include <boost/utility.hpp>
#include <boost/thread/mutex.hpp>


/**
 * @brief boost::threadpool::detail 命名空间包含了线程池库的内部实现细节
 * @details 该命名空间中的类和函数为线程池库的公共接口提供支持，通常不应直接使用
 */
namespace boost { namespace threadpool { namespace detail 
{

/**
 * @brief 带有作用域锁定机制的智能指针
 *
 * @details 该类是对易变指针的包装器。它通过锁定传入的互斥量实现对内部指针的同步访问。
 * 在构造时自动锁定互斥量，在析构时自动解锁，从而确保线程安全的访问模式。
 * @tparam T 指针指向的对象类型
 * @tparam Mutex 互斥量类型，用于同步访问
 */
  template <typename T, typename Mutex>
  class locking_ptr 
  : private noncopyable
  {
    /**
     * @brief 被包装的对象指针
     * @details 指向实际要访问的对象实例
     */
    T* m_obj;
    
    /**
     * @brief 互斥量引用
     * @details 用于实现作用域锁定的互斥量对象引用
     */
    Mutex & m_mutex;

  public:
    /**
     * @brief 构造函数
     * @details 创建一个 locking_ptr 对象，接受要访问的对象和用于同步的互斥量。
     *          构造函数会自动锁定互斥量，确保对象的独占访问。
     * @param obj 被包装的对象的引用，声明为 volatile
     * @param mtx 用于同步访问的互斥量的引用
     */
    locking_ptr(volatile T& obj, const volatile Mutex& mtx)
      : m_obj(const_cast<T*>(&obj))
      , m_mutex(*const_cast<Mutex*>(&mtx))
    {   
      // Lock mutex
	  m_mutex.lock();
    }


    /**
     * @brief 析构函数
     * @details 当 locking_ptr 对象被销毁时自动解锁互斥量。
     *          这确保了在访问对象后释放锁，实现了自动的锁管理。
     */
    ~locking_ptr()
    { 
      // Unlock mutex
      m_mutex.unlock();
    }


    /**
     * @brief 解引用运算符
     * @details 返回被锁定的对象的引用。由于互斥量已经锁定，
     *          所以可以安全地访问该对象而不会发生竞态条件。
     * @return 被包装对象的引用
     */
    T& operator*() const
    {    
      return *m_obj;    
    }


    /**
     * @brief 成员访问运算符
     * @details 返回被锁定对象的指针，允许访问其成员变量和方法。
     *          由于互斥量已经锁定，所以可以安全地访问对象的成员。
     * @return 被包装对象的指针
     */
    T* operator->() const
    {   
      return m_obj;   
    }
  };


} } } // namespace boost::threadpool::detail


#endif // THREADPOOL_DETAIL_LOCKING_PTR_HPP_INCLUDED

