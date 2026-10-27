/*!
 * \file BoostShm.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Boost共享内存组件的封装，方便使用
 * 
 * \details 本文件封装了基于Boost库的共享内存操作功能，提供了以下功能：
 * - 共享内存的创建、打开和关闭
 * - 共享内存的映射和访问
 * - 共享内存大小的获取和设置
 * 
 * 这个类主要基于Boost.Interprocess库中的shared_memory_object和mapped_region组件，
 * 并提供了简化的接口便于进程间共享数据。
 */
#pragma once

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

/**
 * @brief Boost共享内存的封装类
 * 
 * @details BoostShm类封装了boost::interprocess库中的共享内存功能，
 * 提供了简化的接口来创建、打开、关闭和管理进程间共享内存。
 * 使用共享内存可以实现不同进程间的高效数据交换，避免了通过文件或网络传输数据的开销。
 * 在这个类中，共享内存段通过名称识别，可以由一个进程创建并由多个进程打开和访问。
 */
class BoostShm
{
private:
	/**
	 * @brief 共享内存的名称
	 * 
	 * @details 存储当前共享内存段的名称，用于在不同进程间标识同一个共享内存。
	 * 该名称在open或create方法中被设置。
	 */
	std::string	_name;

	/**
	 * @brief 共享内存对象指针
	 * 
	 * @details 指向boost::interprocess::shared_memory_object对象的指针，该对象管理共享内存的创建、打开和属性设置。
	 * 在open或create方法中创建，在close方法中释放。
	 */
	boost::interprocess::shared_memory_object*	_obj;

	/**
	 * @brief 内存映射区域指针
	 * 
	 * @details 指向boost::interprocess::mapped_region对象的指针，该对象表示实际的内存映射区域。
	 * 它包含了共享内存的地址和大小信息，可以通过addr()和size()方法访问。
	 * 在open或create方法中创建，在close方法中释放。
	 */
	boost::interprocess::mapped_region *		_region;

public:
	/**
	 * @brief 默认构造函数
	 * 
	 * @details 创建一个新的BoostShm实例，并将共享内存对象和映射区域指针初始化为nullptr。
	 * 创建对象后需要调用open或create方法才能使用共享内存。
	 */
	BoostShm(): _obj(nullptr), _region(nullptr){}

	/**
	 * @brief 析构函数
	 * 
	 * @details 析构BoostShm实例时自动关闭共享内存。
	 * 调用close()方法来释放所有相关资源，确保共享内存对象和映射区域对象被正确删除。
	 * 这样可以保证在对象离开作用域时不会有资源泄漏。
	 */
	~BoostShm()
	{
		close();
	}

	/**
	 * @brief 关闭共享内存
	 * 
	 * @details 关闭当前的共享内存，释放相关资源。
	 * 该方法会按照以下顺序执行操作：
	 * 1. 首先删除映射区域对象（_region），这会取消共享内存到进程内存的映射
	 * 2. 然后删除共享内存对象（_obj）
	 * 3. 最后将两个指针的值设置为nullptr，防止重复释放或访问无效指针
	 * 该方法可以手动调用以释放资源，也会在析构函数中自动调用。
	 * 注意这个方法只释放当前进程与共享内存的连接，而不会删除共享内存中的数据或者系统中的共享内存对象本身。
	 */
	void close()
	{
		if (_region)
			delete _region;

		if (_obj)
			delete _obj;

		_obj = nullptr;
		_region = nullptr;
	}

	/**
	 * @brief 打开现有的共享内存
	 * 
	 * @param name 要打开的共享内存的名称
	 * @return bool 打开成功返回true，失败返回false
	 * 
	 * @details 打开一个已存在的共享内存段，并将其映射到当前进程的地址空间中。
	 * 这个方法执行以下操作：
	 * 1. 使用open_only模式创建共享内存对象，该模式要求共享内存已经存在
	 * 2. 创建映射区域，将共享内存映射到进程的地址空间
	 * 
	 * 如果操作成功，返回true。如果失败（例如共享内存不存在或无法访问），返回false。
	 * 该方法使用try-catch结构来捕获可能发生的异常，确保即使操作失败也能优雅地处理。
	 * 
	 * 注意：在使用这个方法前，必须确保共享内存已经被其他进程通过create方法创建。
	 */
	bool open(const char* name)
	{
		try
		{
			_obj = new boost::interprocess::shared_memory_object(boost::interprocess::open_only, name, boost::interprocess::read_write);
			_region = new boost::interprocess::mapped_region(*_obj, boost::interprocess::read_write);

			return true;
		}
		catch(...)
		{
			return false;
		}
	}

	/**
	 * @brief 创建新的共享内存
	 * 
	 * @param name 要创建的共享内存的名称
	 * @param size 要创建的共享内存的大小（字节数）
	 * @return bool 创建成功返回true，失败返回false
	 * 
	 * @details 创建一个新的共享内存段，并将其映射到当前进程的地址空间中。
	 * 这个方法执行以下操作：
	 * 1. 首先尝试删除同名的共享内存（如果存在），以确保创建新的共享内存段
	 * 2. 使用create_only模式创建共享内存对象，该模式要求共享内存不存在
	 * 3. 调用truncate方法设置共享内存的大小
	 * 4. 创建映射区域，将共享内存映射到进程的地址空间
	 * 
	 * 如果操作成功，返回true。如果失败（例如没有权限创建共享内存），返回false。
	 * 该方法使用try-catch结构来捕获可能发生的异常，确保即使操作失败也能优雅地处理。
	 * 
	 * 注意：创建共享内存后，其他进程可以通过open方法打开并访问它。
	 */
	bool create(const char* name, std::size_t size)
	{
		try
		{
			boost::interprocess::shared_memory_object::remove(name);
			_obj = new boost::interprocess::shared_memory_object(boost::interprocess::create_only, name, boost::interprocess::read_write);
			_obj->truncate(size);
			_region = new boost::interprocess::mapped_region(*_obj, boost::interprocess::read_write);

			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	/**
	 * @brief 获取共享内存的地址
	 * 
	 * @return void* 共享内存映射区域的起始地址指针，如果映射区域不存在则返回nullptr
	 * 
	 * @details 获取共享内存在当前进程中的起始地址。
	 * 这个方法返回一个指向内存映射区域开始处的指针，可以通过这个指针直接访问和操作共享内存的内容。
	 * 如果映射区域对象不存在（即_region为nullptr），则返回nullptr。
	 * 返回的指针可以进行类型转换，以适应不同类型的数据访问需求。
	 * 
	 * 该函数被声明为inline，以提高性能，减少函数调用开销。
	 */
	inline void *addr()
	{
		if (_region)
			return _region->get_address();
		return nullptr;
	}

	/**
	 * @brief 获取共享内存的大小
	 * 
	 * @return size_t 共享内存映射区域的字节数，如果映射区域不存在则返回0
	 * 
	 * @details 获取共享内存段的大小（字节数）。
	 * 这个方法返回当前映射到进程内存中的共享内存区域的大小。
	 * 如果映射区域对象不存在（即_region为nullptr），则返回0。
	 * 这个大小信息对于确定可以访问的共享内存区域范围非常重要，可以避免越界访问。
	 * 
	 * 该函数被声明为inline，以提高性能，减少函数调用开销。
	 */
	inline size_t size()
	{
		if (_region)
			return _region->get_size();
		return 0;
	}

	/**
	 * @brief 检查共享内存是否有效
	 * 
	 * @return bool 如果共享内存对象有效返回true，否则返回false
	 * 
	 * @details 检查当前的共享内存对象是否有效。
	 * 这个方法通过检查共享内存对象指针（_obj）是否为nullptr来判断共享内存是否有效。
	 * 如果_obj不为nullptr，则表示已经成功创建或打开了共享内存。
	 * 这个方法可以在访问共享内存之前调用，以确保共享内存已经正确初始化。
	 * 
	 * 该函数被声明为inline和const，表示它不会修改对象的内部状态，并且会在编译时被内联展开以提高性能。
	 */
	inline bool valid() const
	{
		return _obj != nullptr;
	}
};