/*!
 * \file ShmCaster.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 共享内存行情广播器定义
 */

#pragma once
#include "IDataCaster.h"
#include <stdint.h>
#include "../Includes/WTSStruct.h"
#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 基于共享内存的行情广播器
 * @details 实现IDataCaster接口，将行情数据广播到共享内存中，供其他进程读取。
 * 使用内存映射文件实现进程间通信，通过对同一文件进行读写操作来传递数据。
 */
class ShmCaster : public IDataCaster
{
public:
#pragma pack(push, 8)
	/**
	 * @brief 数据项结构体，用于存储各种类型的行情数据
	 * @details 包含数据类型标识和一个联合体，该联合体根据数据类型存储不同类型的行情数据
	 */
	typedef struct _DataItem
	{
		/**
		 * @brief 数据类型标识
		 * @details 0-tick行情数据, 1-委托队列数据, 2-逆序委托数据, 3-逆序成交数据
		 */
		uint32_t	_type;
		/**
		 * @brief 不同类型数据的存储联合体
		 */
		union
		{
			WTSTickStruct	_tick;      ///< Tick行情数据结构
			WTSOrdQueStruct _queue;    ///< 委托队列数据结构
			WTSOrdDtlStruct	_order;  ///< 逆序委托数据结构
			WTSTransStruct	_trans;   ///< 逆序成交数据结构
		};

		/**
		 * @brief 构造函数，初始化全部数据为0
		 */
		_DataItem() { memset(this, 0, sizeof(_DataItem)); }
	} DataItem;

	/**
	 * @brief 共享内存数据队列模板
	 * @details 基于环形缓冲区实现的数据队列，用于在进程间安全地传递数据
	 * @tparam N 队列容量，默认8*1024个数据项
	 */
	template <int N = 8*1024>
	struct _DataQueue
	{
		/// @brief 队列容量
		uint64_t	_capacity = N;
		/// @brief 可读索引，远程进程读取时使用，定义为volatile确保多进程可见性
		volatile uint64_t	_readable;
		/// @brief 可写索引，本地进程写入时使用，定义为volatile确保多进程可见性
		volatile uint64_t	_writable;
		/// @brief 创建该队列的进程标识
		uint32_t	_pid;
		/// @brief 数据项数组，存储实际的行情数据
		DataItem	_items[N];

		/**
		 * @brief 构造函数，初始化队列状态
		 * @details 将可读索引初始化为最大值，表示没有可读数据，将可写索引和进程标识初始化为0
		 */
		_DataQueue() :_readable(UINT64_MAX), _writable(0), _pid(0) {}
	};

	/**
	 * @brief 广播队列类型别名，容量固定为8*1024个数据项
	 */
	typedef _DataQueue<8*1024>	CastQueue;

#pragma pack(pop)

public:
	/**
	 * @brief 构造函数
	 * @details 初始化成员变量，将队列指针设置为空，初始化状态设置为未初始化
	 */
	ShmCaster():_queue(NULL), _inited(false){}

	/**
	 * @brief 初始化广播器
	 * @details 根据配置初始化共享内存广播器，创建或打开内存映射文件
	 * @param cfg 配置项指针，需要包含"path"和"active"字段
	 * @return bool 初始化是否成功
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 广播股票行情数据
	 * @details 将Tick数据写入共享内存队列中，供其他进程读取
	 * @param curTick Tick数据指针
	 */
	virtual void	broadcast(WTSTickData* curTick) override;
	/**
	 * @brief 广播委托队列数据
	 * @details 将委托队列数据写入共享内存队列中，供其他进程读取
	 * @param curOrdQue 委托队列数据指针
	 */
	virtual void	broadcast(WTSOrdQueData* curOrdQue) override;
	/**
	 * @brief 广播逆序委托明细数据
	 * @details 将逆序委托明细数据写入共享内存队列中，供其他进程读取
	 * @param curOrdDtl 逆序委托明细数据指针
	 */
	virtual void	broadcast(WTSOrdDtlData* curOrdDtl) override;
	/**
	 * @brief 广播逆序逆序成交数据
	 * @details 将逆序成交数据写入共享内存队列中，供其他进程读取
	 * @param curTrans 逆序成交数据指针
	 */
	virtual void	broadcast(WTSTransData* curTrans) override;

private:
	/// @brief 共享内存映射文件路径
	std::string		_path;
	/// @brief 内存映射文件智能指针类型别名
	typedef std::shared_ptr<BoostMappingFile> MappedFilePtr;
	/// @brief 内存映射文件智能指针
	MappedFilePtr	_mapfile;
	/// @brief 广播队列指针，指向内存映射区域
	CastQueue*		_queue;
	/// @brief 是否已成功初始化标记
	bool			_inited;
};

