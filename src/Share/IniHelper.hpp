/**
 * @file IniHelper.hpp
 * @brief Ini文件辅助类,利用boost的property_tree来实现,可以跨平台使用
 * @details 此文件提供了对INI配置文件的读写操作，支持不同类型的数据读写，以及节和键的管理
 * @author Wesley
 * @date 2020/03/30
 */
#pragma once

#include <string>
#include <vector>
#include <map>

#include <boost/property_tree/ptree.hpp>  
#include <boost/property_tree/ini_parser.hpp>

/**
 * @brief 字符串数组类型定义
 * @details 用于存储从配置文件中读取的节名、键名或值的集合
 */
typedef std::vector<std::string>			FieldArray;

/**
 * @brief 字符串映射表类型定义
 * @details 用于存储键值对的映射关系
 */
typedef std::map<std::string, std::string>	FieldMap;

/**
 * @brief INI配置文件辅助类
 * @details 封装了对INI配置文件的各种操作，包括读写不同类型的值、获取节和键的列表等
 *          基于boost库的property_tree实现，提供了跨平台的INI文件处理能力
 */
class IniHelper
{
private:
	/**
	 * @brief 配置文件的属性树
	 * @details 存储整个INI文件的内容结构
	 */
	boost::property_tree::ptree	_root;
	
	/**
	 * @brief 配置文件名
	 * @details 存储当前加载的INI文件路径
	 */
	std::string					_fname;
	
	/**
	 * @brief 是否已加载标志
	 * @details 指示配置文件是否已成功加载
	 */
	bool						_loaded;

	/**
	 * @brief 键路径的最大长度
	 * @details 用于内部构建节名和键名组合路径的缓冲区大小限制
	 */
	static const uint32_t MAX_KEY_LENGTH = 64;

public:
	/**
	 * @brief 构造函数
	 * @details 初始化INI配置文件辅助类对象
	 */
	IniHelper(): _loaded(false){}

	/**
	 * @brief 加载配置文件
	 * @param szFile 配置文件路径
	 * @details 从指定文件路径加载INI配置文件
	 */
	void	load(const char* szFile)
	{
		_fname = szFile;
		try
		{
			boost::property_tree::ini_parser::read_ini(szFile, _root);
		}
		catch(...)
		{

		}
		
		_loaded = true;
	}

	/**
	 * @brief 保存配置文件
	 * @param filename 目标文件路径，默认为空字符串
	 * @details 将当前内存中的配置保存到文件中
	 *          如果提供了文件名，则保存到指定文件
	 *          否则保存到原始加载的文件中
	 */
	void	save(const char* filename = "")
	{
		if (strlen(filename) > 0)
			boost::property_tree::ini_parser::write_ini(filename, _root);
		else
			boost::property_tree::ini_parser::write_ini(_fname.c_str(), _root);
	}

	/**
	 * @brief 检查配置文件是否已加载
	 * @return bool 如果文件已加载返回true，否则返回false
	 * @details 查询当前对象是否已成功加载了配置文件
	 */
	inline bool isLoaded() const{ return _loaded; }

public:
	/**
	 * @brief 移除指定节下的指定键
	 * @param szSec 节名
	 * @param szKey 键名
	 * @details 从配置文件中移除指定节下的指定键
	 *          如果节或键不存在，操作将被忽略
	 */
	void	removeValue(const char* szSec, const char* szKey)
	{
		try
		{
			boost::property_tree::ptree& sec = _root.get_child(szSec);
			sec.erase(szKey);
		}
		catch (...)
		{
			
		}
	}

	/**
	 * @brief 移除指定节
	 * @param szSec 节名
	 * @details 从配置文件中移除指定的节，包括该节下的所有键
	 *          如果节不存在，操作将被忽略
	 */
	void	removeSection(const char* szSec)
	{
		try
		{
			_root.erase(szSec);
		}
		catch (...)
		{

		}
	}

	/**
	 * @brief 读取指定路径的值
	 * @tparam T 要读取的值类型
	 * @param szPath 完整路径，格式为“节.键”
	 * @param defVal 默认值，当路径不存在或读取失败时返回
	 * @return T 读取到的值或默认值
	 * @details 该函数是各种类型读取函数的核心实现
	 *          直接使用完整路径读取对应类型的值
	 *          如果读取失败，将返回提供的默认值
	 */
	template<class T>
	T	readValue(const char* szPath, T defVal)
	{
		try
		{
			return _root.get<T>(szPath, defVal);
		}
		catch (...)
		{
			return defVal;
		}
	}

	/**
	 * @brief 读取字符串值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param defVal 默认值，默认为空字符串
	 * @return std::string 读取到的字符串值或默认值
	 * @details 读取指定节下指定键的字符串值
	 *          如果节或键不存在，将返回默认值
	 */
	std::string	readString(const char* szSec, const char* szKey, const char* defVal = "")
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		return readValue<std::string>(path, defVal);
	}

	/**
	 * @brief 读取整数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param defVal 默认值，默认为0
	 * @return int 读取到的整数值或默认值
	 * @details 读取指定节下指定键的整数值
	 *          如果节或键不存在，或者值无法转换为整数，将返回默认值
	 */
	int			readInt(const char* szSec, const char* szKey, int defVal = 0)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		return readValue<int>(path, defVal);
	}

	/**
	 * @brief 读取无符号整数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param defVal 默认值，默认为0
	 * @return uint32_t 读取到的无符号整数值或默认值
	 * @details 读取指定节下指定键的无符号整数值
	 *          如果节或键不存在，或者值无法转换为无符号整数，将返回默认值
	 */
	uint32_t	readUInt(const char* szSec, const char* szKey, uint32_t defVal = 0)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		return readValue<uint32_t>(path, defVal);
	}

	/**
	 * @brief 读取布尔值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param defVal 默认值，默认为false
	 * @return bool 读取到的布尔值或默认值
	 * @details 读取指定节下指定键的布尔值
	 *          如果节或键不存在，或者值无法转换为布尔值，将返回默认值
	 *          布尔值的识别基于 boost 的规则，通常识别 "true"/"false" 或 "1"/"0"
	 */
	bool		readBool(const char* szSec, const char* szKey, bool defVal = false)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		return readValue<bool>(path, defVal);
	}

	/**
	 * @brief 读取浮点数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param defVal 默认值，默认为0.0
	 * @return double 读取到的浮点数值或默认值
	 * @details 读取指定节下指定键的浮点数值
	 *          如果节或键不存在，或者值无法转换为浮点数，将返回默认值
	 */
	double		readDouble(const char* szSec, const char* szKey, double defVal = 0.0)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		return readValue<double>(path, defVal);
	}

	/**
	 * @brief 读取所有节的名称
	 * @param aySection 输出参数，用于存储读取到的节名数组
	 * @return int 读取到的节的数量
	 * @details 该函数遍历配置文件中的所有节，将节名添加到传入的数组中
	 *          返回值为遍历到的节的数量
	 */
	int			readSections(FieldArray &aySection)
	{
		for (auto it = _root.begin(); it != _root.end(); it++)
		{
			aySection.emplace_back(it->first.data());
		}

		return (int)_root.size();
	}

	/**
	 * @brief 读取指定节下的所有键名
	 * @param szSec 要读取的节名
	 * @param ayKey 输出参数，用于存储读取到的键名数组
	 * @return int 读取到的键的数量，如果节不存在则返回0
	 * @details 该函数遍历指定节下的所有键，将键名添加到传入的数组中
	 *          如果指定的节不存在，则返回0，且不会修改传入的数组
	 */
	int			readSecKeyArray(const char* szSec, FieldArray &ayKey)
	{
		try
		{
			const boost::property_tree::ptree& _sec = _root.get_child(szSec);
			for (auto it = _sec.begin(); it != _sec.end(); it++)
			{
				ayKey.emplace_back(it->first.data());
			}

			return (int)_sec.size();
		}
		catch (...)
		{
			return 0;
		}
		
	}

	/**
	 * @brief 读取指定节下的所有键值对
	 * @param szSec 要读取的节名
	 * @param ayKey 输出参数，用于存储读取到的键名数组
	 * @param ayVal 输出参数，用于存储读取到的键值数组，与键名数组一一对应
	 * @return int 读取到的键值对数量，如果节不存在则返回0
	 * @details 该函数遍历指定节下的所有键值对，将键名和键值分别添加到传入的两个数组中
	 *          如果指定的节不存在，则返回0，且不会修改传入的数组
	 *          ayKey和ayVal数组中的元素与配置文件中的键值对一一对应
	 */
	int			readSecKeyValArray(const char* szSec, FieldArray &ayKey, FieldArray &ayVal)
	{
		try
		{
			const boost::property_tree::ptree& _sec = _root.get_child(szSec);
			for (auto it = _sec.begin(); it != _sec.end(); it++)
			{
				ayKey.emplace_back(it->first.data());
				ayVal.emplace_back(it->second.data());
			}

			return (int)_sec.size();
		}
		catch (...)
		{
			return 0;
		}
	}

	/**
	 * @brief 写入指定路径的值
	 * @tparam T 要写入的值类型
	 * @param szPath 完整路径，格式为“节.键”
	 * @param val 要写入的值
	 * @details 该函数是各种类型写入函数的核心实现
	 *          直接使用完整路径写入对应类型的值
	 */
	template<class T>
	void		writeValue(const char* szPath, T val)
	{
		_root.put<T>(szPath, val);
	}

	/**
	 * @brief 写入字符串值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param val 要写入的字符串值
	 * @details 将字符串值写入到指定节下的指定键
	 *          如果节或键不存在，将自动创建
	 *          如果键已存在，将覆盖原有的值
	 */
	void		writeString(const char* szSec, const char* szKey, const char* val)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		writeValue<std::string>(path, val);
	}

	/**
	 * @brief 写入整数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param val 要写入的整数值
	 * @details 将整数值写入到指定节下的指定键
	 *          如果节或键不存在，将自动创建
	 *          如果键已存在，将覆盖原有的值
	 */
	void		writeInt(const char* szSec, const char* szKey, int val)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		writeValue<int>(path, val);
	}

	/**
	 * @brief 写入无符号整数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param val 要写入的无符号整数值
	 * @details 将无符号整数值写入到指定节下的指定键
	 *          如果节或键不存在，将自动创建
	 *          如果键已存在，将覆盖原有的值
	 */
	void		writeUInt(const char* szSec, const char* szKey, uint32_t val)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		writeValue<uint32_t>(path, val);
	}

	/**
	 * @brief 写入布尔值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param val 要写入的布尔值
	 * @details 将布尔值写入到指定节下的指定键
	 *          如果节或键不存在，将自动创建
	 *          如果键已存在，将覆盖原有的值
	 *          布尔值将被保存为"true"或"false"
	 */
	void		writeBool(const char* szSec, const char* szKey, bool val)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		writeValue<bool>(path, val);
	}

	/**
	 * @brief 写入浮点数值
	 * @param szSec 节名
	 * @param szKey 键名
	 * @param val 要写入的浮点数值
	 * @details 将浮点数值写入到指定节下的指定键
	 *          如果节或键不存在，将自动创建
	 *          如果键已存在，将覆盖原有的值
	 */
	void		writeDouble(const char* szSec, const char* szKey, double val)
	{
		static char path[MAX_KEY_LENGTH] = { 0 };
		sprintf(path, "%s.%s", szSec, szKey);
		writeValue<double>(path, val);
	}
};