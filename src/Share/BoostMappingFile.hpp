/*!
 * \file BoostMappingFile.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief boost的内存映射文件组件的封装,方便使用
 */
#pragma once
#include <boost/filesystem.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

/**
 * @brief Boost内存映射文件的封装类
 * 
 * @details BoostMappingFile类封装了boost::interprocess库中的内存映射文件功能，
 * 提供了简化的接口来创建、访问和管理内存映射文件。
 * 内存映射文件允许将磁盘上的文件直接映射到进程的地址空间，
 * 从而可以像访问内存一样访问文件内容，避免了传统文件IO操作的开销。
 * 这个类尤其适用于需要高效访问大型文件的场景。
 */
class BoostMappingFile
{
public:
	/**
	 * @brief 默认构造函数
	 * 
	 * @details 创建一个新的BoostMappingFile实例，并将文件映射和映射区域指针初始化为NULL。
	 * 创建对象后需要调用map方法才能映射一个实际的文件到内存中进行操作。
	 */
	BoostMappingFile()
	{
		_file_map=NULL;
		_map_region=NULL;
	}

	/**
	 * @brief 析构函数
	 * 
	 * @details 析构BoostMappingFile实例时自动关闭内存映射文件。
	 * 调用close()方法来释放所有相关资源，确保映射区域和文件映射对象被正确删除。
	 * 这样可以保证在对象离开作用域时不会有资源泄漏。
	 */
	~BoostMappingFile()
	{
		close();
	}

	/**
	 * @brief 关闭内存映射文件
	 * 
	 * @details 关闭当前的内存映射文件，释放相关资源。
	 * 该方法会按照以下顺序执行操作：
	 * 1. 首先删除映射区域对象（_map_region），这会取消文件到内存的映射
	 * 2. 然后删除文件映射对象（_file_map）
	 * 3. 最后将两个指针的值设置为NULL，防止重复释放或访问无效指针
	 * 该方法可以手动调用以释放资源，也会在析构函数中自动调用。
	 */
	void close()
	{
		if(_map_region!=NULL)
			delete _map_region;

		if(_file_map!=NULL)
			delete _file_map;

		_file_map=NULL;
		_map_region=NULL;
	}

	/**
	 * @brief 同步内存映射文件的内容到磁盘
	 * 
	 * @details 将内存映射的文件内容同步到磁盘上。
	 * 当对内存映射区域进行写操作后，可以调用此方法确保修改被写入到磁盘。
	 * 该方法通过调用映射区域对象的flush()方法来实现数据的同步。
	 * 如果映射区域对象不存在（即_map_region为NULL），则不进行任何操作。
	 */
	void sync()
	{
		if(_map_region)
			_map_region->flush();
	}

	/**
	 * @brief 获取内存映射区域的地址
	 * 
	 * @return void* 内存映射区域的起始地址指针，如果映射区域不存在则返回NULL
	 * 
	 * @details 获取内存映射文件在内存中的起始地址。
	 * 这个方法返回一个指向内存映射区域开始处的指针，可以通过这个指针直接访问和操作文件内容。
	 * 如果映射区域对象不存在（即_map_region为NULL），则返回NULL指针。
	 * 返回的指针可以进行类型转换，以适应不同类型的数据访问需求。
	 */
	void *addr()
	{
		if(_map_region)
			return _map_region->get_address();
		return NULL;
	}

	/**
	 * @brief 获取内存映射区域的大小
	 * 
	 * @return size_t 内存映射区域的字节数，如果映射区域不存在则返回0
	 * 
	 * @details 获取内存映射文件的大小（字节数）。
	 * 这个方法返回当前映射到内存中的文件区域的大小。
	 * 如果映射区域对象不存在（即_map_region为NULL），则返回0。
	 * 这个大小信息对于确定可以访问的映射区域范围非常重要，可以避免越界访问。
	 */
	size_t size()
	{
		if(_map_region)
			return _map_region->get_size();
		return 0;
	}

	/**
	 * @brief 将文件映射到内存
	 * 
	 * @param filename 要映射的文件路径
	 * @param mode 文件打开模式，默认为read_write（读写模式）
	 * @param mapmode 内存映射模式，默认为read_write（读写模式）
	 * @param zeroother 未使用的参数，保留作向后兼容
	 * @return bool 映射成功返回true，失败返回false
	 * 
	 * @details 将指定的文件映射到内存中，使其可以直接通过内存访问。
	 * 这个方法执行以下操作：
	 * 1. 首先检查文件是否存在，如果不存在则返回false
	 * 2. 保存文件名到_file_name变量
	 * 3. 创建文件映射对象（file_mapping），使用指定的文件和访问模式
	 * 4. 创建映射区域对象（mapped_region），将文件内容实际映射到内存
	 * 5. 如果任何一步创建失败，则释放已分配的资源并返回false
	 * 
	 * 参数mode和mapmode可以使用以下值：
	 * - boost::interprocess::read_only：只读模式
	 * - boost::interprocess::read_write：读写模式
	 */
	bool map(const char *filename,
		int mode=boost::interprocess::read_write,
		int mapmode=boost::interprocess::read_write,bool zeroother=true)
	{
		if (!boost::filesystem::exists(filename))
		{
			return false;
		}
		_file_name = filename;

		_file_map = new boost::interprocess::file_mapping(filename,(boost::interprocess::mode_t)mode);
		if(_file_map==NULL)
			return false;

		_map_region = new boost::interprocess::mapped_region(*_file_map,(boost::interprocess::mode_t)mapmode);
		if(_map_region==NULL)
		{
			delete _file_map;
			return false;
		}

		return true;
	}

	/**
	 * @brief 获取当前映射的文件名
	 * 
	 * @return const char* 当前映射文件的文件名（包含路径）
	 * 
	 * @details 返回当前内存映射文件的文件名。
	 * 这个方法返回映射到内存中的文件的完整路径名称。
	 * 返回的是内部_file_name字符串的C风格字符串指针（通过c_str()方法获取）。
	 * 如果还没有映射文件，则返回空字符串。
	 */
	const char* filename()
	{
		return _file_name.c_str();
	}

	/**
	 * @brief 检查内存映射文件是否有效
	 * 
	 * @return bool 如果内存映射文件有效返回true，否则返回false
	 * 
	 * @details 检查当前的内存映射是否有效。
	 * 这个方法通过检查文件映射对象指针（_file_map）是否为NULL来判断内存映射是否有效。
	 * 如果_file_map不为NULL，则表示已经成功映射了文件到内存。
	 * 这个方法可以在访问内存映射文件之前调用，以确保映射文件已经正确初始化。
	 */
	bool valid() const
	{
		return _file_map != NULL;
	}

private:
	/**
	 * @brief 映射文件的路径名称
	 * 
	 * @details 存储当前被映射到内存中的文件的完整路径名称。
	 * 这个变量在map方法中被设置，并可以通过filename()方法获取。
	 */
	std::string _file_name;

	/**
	 * @brief 文件映射对象指针
	 * 
	 * @details 指向boost::interprocess::file_mapping对象的指针，该对象负责将文件与内存映射关联。
	 * 在map方法中创建，在close方法中释放。如果为NULL，表示未有映射文件。
	 */
	boost::interprocess::file_mapping *_file_map;

	/**
	 * @brief 映射区域对象指针
	 * 
	 * @details 指向boost::interprocess::mapped_region对象的指针，该对象表示实际的内存映射区域。
	 * 它包含映射区域的地址和大小信息，可以通过addr()和size()方法访问。
	 * 在map方法中创建，在close方法中释放。
	 */
	boost::interprocess::mapped_region *_map_region;
};

