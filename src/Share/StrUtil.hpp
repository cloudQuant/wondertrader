/*!
 * \file StrUtil.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 字符串处理工具类
 * 
 * 该文件提供了字符串处理的常用函数，包括字符串的修剪、分割、大小写转换等常用操作。
 */
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <functional>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <cctype>

/**
 * @brief 字符串数组类型
 * 
 * 便于存储多个字符串，通常用于字符串分割等操作的结果存储
 */
typedef std::vector<std::string> StringVector;

/**
 * @brief 字符串工具类
 * 
 * 提供各种字符串相关的处理函数，包括但不限于：字符串修剪、分割、大小写转换、匹配等功能
 */

class StrUtil
{
public:

	

	/**
	 * @brief 修剪字符串两端的指定字符
	 * 
	 * @param str 要修剪的字符串引用
	 * @param delims 要移除的字符集合，默认为空格、制表符和回车
	 * @param left 是否修剪左端，默认为true
	 * @param right 是否修剪右端，默认为true
	 */
	static inline void trim(std::string& str, const char* delims = " \t\r", bool left = true, bool right = true)
	{
		if(right)
			str.erase(str.find_last_not_of(delims)+1);
		if(left)
			str.erase(0, str.find_first_not_of(delims));
	}

	/**
	 * @brief 修剪字符串两端的指定字符
	 * 
	 * @param str 要修剪的字符串
	 * @param delims 要移除的字符集合，默认为空白字符（空格、换页、换行、回车、制表符、垂直制表符）
	 * @param left 是否修剪左端，默认为true
	 * @param right 是否修剪右端，默认为true
	 * @return std::string 修剪后的字符串
	 */
	static inline std::string trim(const char* str, const char* delims = " \t\r", bool left = true, bool right = true)
	{
		std::string ret = str;
		if(right)
			ret.erase(ret.find_last_not_of(delims)+1);
		if(left)
			ret.erase(0, ret.find_first_not_of(delims));

		return std::move(ret);
	}

	/**
	 * @brief 去除字符串中的所有空格字符
	 * 
	 * @param str 要处理的字符串引用，处理后直接修改原字符串
	 */
	static inline void trimAllSpace(std::string &str)
	{
		std::string::iterator destEnd = std::remove_if(str.begin(), str.end(), [](const char& c){
			return c == ' ';
		});
		str.resize(destEnd-str.begin());
	}

	//去除所有特定字符
	//static inline void trimAll(std::string &str,char ch)
	//{
	//	std::string::iterator destEnd=std::remove_if(str.begin(),str.end(),std::bind1st(std::equal_to<char>(),ch));
	//	str.resize(destEnd-str.begin());
	//}

	/**
	 * @brief 查找字符在字符串中的第一次出现位置
	 * 
	 * @param str 要搜索的字符串
	 * @param ch 要查找的字符
	 * @return std::size_t 字符首次出现的位置索引，如果未找到则返回std::string::npos
	 */
	static inline std::size_t findFirst(const char* str, char ch)
	{
		std::size_t i = 0;
		for(;;)
		{
			if (str[i] == ch)
				return i;

			if(str[i] == '\0')
				break;

			i++;
		}

		return std::string::npos;
	}

	/**
	 * @brief 查找字符在字符串中的最后一次出现位置
	 * 
	 * @param str 要搜索的字符串
	 * @param ch 要查找的字符
	 * @return std::size_t 字符最后一次出现的位置索引，如果未找到则返回std::string::npos
	 */
	static inline std::size_t findLast(const char* str, char ch)
	{
		auto len = strlen(str);
		std::size_t i = 0;
		for (; i < len; i++)
		{
			if (str[len - 1 - i] == ch)
				return len - 1 - i;
		}

		return std::string::npos;
	}

	/**
	 * @brief 分割字符串为多个字符串并返回字符串数组
	 * 
	 * @param str 要分割的字符串
	 * @param delims 分割符列表，默认为制表符、换行和空格
	 * @param maxSplits 最大分割数量（0表示不限制）。如果设置为大于0的值，则分割过程将在分割达到这个数量后停止（从左到右）
	 * @return StringVector 分割后的字符串数组
	 */
	static inline StringVector split( const std::string& str, const std::string& delims = "\t\n ", unsigned int maxSplits = 0)
	{
		StringVector ret;
		unsigned int numSplits = 0;

		// Use STL methods
		size_t start, pos;
		start = 0;
		do
		{
			pos = str.find_first_of(delims, start);
			if (pos == start)
			{
				ret.emplace_back("");
				// Do nothing
				start = pos + 1;
			}
			else if (pos == std::string::npos || (maxSplits && numSplits == maxSplits))
			{
				// Copy the rest of the std::string
				ret.emplace_back( str.substr(start) );
				break;
			}
			else
			{
				// Copy up to delimiter
				ret.emplace_back( str.substr(start, pos - start) );
				start = pos + 1;
			}
			// parse up to next real data
			//start = str.find_first_not_of(delims, start);
			++numSplits;

		} while (pos != std::string::npos);
		return std::move(ret);
	}

	/**
	 * @brief 分割字符串为多个字符串，并将结果存储到指定的数组中
	 * 
	 * @param str 要分割的字符串
	 * @param ret 存储分割结果的字符串数组引用
	 * @param delims 分割符列表，默认为制表符、换行和空格
	 * @param maxSplits 最大分割数量（0表示不限制）。如果设置为大于0的值，则分割过程将在分割达到这个数量后停止（从左到右）
	 */
	static inline void split(const std::string& str, StringVector& ret, const std::string& delims = "\t\n ", unsigned int maxSplits = 0)
	{
		unsigned int numSplits = 0;

		// Use STL methods
		size_t start, pos;
		start = 0;
		do
		{
			pos = str.find_first_of(delims, start);
			if (pos == start)
			{
				ret.emplace_back("");
				// Do nothing
				start = pos + 1;
			}
			else if (pos == std::string::npos || (maxSplits && numSplits == maxSplits))
			{
				// Copy the rest of the std::string
				ret.emplace_back(str.substr(start));
				break;
			}
			else
			{
				// Copy up to delimiter
				ret.emplace_back(str.substr(start, pos - start));
				start = pos + 1;
			}
			// parse up to next real data
			//start = str.find_first_not_of(delims, start);
			++numSplits;

		} while (pos != std::string::npos);
	}

	/**
	 * @brief 将字符串转换为小写
	 * 
	 * @param str 要转换的字符串引用，转换后直接修改原字符串
	 */
	static inline void toLowerCase( std::string& str )
	{
		std::transform(
			str.begin(),
			str.end(),
			str.begin(),
			(int(*)(int))tolower);

	}

	/**
	 * @brief 将字符串转换为大写
	 * 
	 * @param str 要转换的字符串引用，转换后直接修改原字符串
	 */
	static inline void toUpperCase( std::string& str )
	{
		std::transform(
			str.begin(),
			str.end(),
			str.begin(),
			(int(*)(int))toupper);
	}

	/**
	 * @brief 将字符串转换为小写并返回新的字符串
	 * 
	 * @param str 要转换的字符串
	 * @return std::string 转换后的字符串副本
	 */
	static inline std::string makeLowerCase(const char* str)
	{
		std::string strRet = str;
		std::transform(
			strRet.begin(),
			strRet.end(),
			strRet.begin(),
			(int(*)(int))tolower);
		return std::move(strRet);
	}

	/**
	 * @brief 将字符串转换为大写并返回新的字符串
	 * 
	 * @param str 要转换的字符串
	 * @return std::string 转换后的字符串副本
	 */
	static inline std::string makeUpperCase(const char* str)
	{
		std::string strRet = str;
		std::transform(
			strRet.begin(),
			strRet.end(),
			strRet.begin(),
			(int(*)(int))toupper);
		return std::move(strRet);
	}

	/**
	 * @brief 检查字符串是否以指定的模式开头
	 * 
	 * @param str 要检查的字符串
	 * @param pattern 要匹配的模式字符串
	 * @param ignoreCase 是否忽略大小写，默认为true
	 * @return bool 如果字符串以指定模式开头则返回true，否则返回false
	 */
	static inline bool startsWith(const char* str, const char* pattern, bool ignoreCase = true)
	{
		size_t thisLen = strlen(str);
		size_t patternLen = strlen(pattern);
		if (thisLen < patternLen || patternLen == 0)
			return false;

		if(ignoreCase)
		{
#ifdef _MSC_VER
			return _strnicmp(str, pattern, patternLen) == 0;
#else
			return strncasecmp(str, pattern, patternLen) == 0;
#endif
		}
		else
		{
			return strncmp(str, pattern, patternLen) == 0;
		}
	}

	/**
	 * @brief 检查字符串是否以指定的模式结尾
	 * 
	 * @param str 要检查的字符串
	 * @param pattern 要匹配的模式字符串
	 * @param ignoreCase 是否忽略大小写，默认为true
	 * @return bool 如果字符串以指定模式结尾则返回true，否则返回false
	 */
	static inline bool endsWith(const char* str, const char* pattern, bool ignoreCase = true)
	{
		size_t thisLen = strlen(str);
		size_t patternLen = strlen(pattern);
		if (thisLen < patternLen || patternLen == 0)
			return false;

		const char* s = str + (thisLen - patternLen);

		if (ignoreCase)
		{
#ifdef _MSC_VER
			return _strnicmp(s, pattern, patternLen) == 0;
#else
			return strncasecmp(s, pattern, patternLen) == 0;
#endif
		}
		else
		{
			return strncmp(s, pattern, patternLen) == 0;
		}
	}

	/**
	 * @brief 标准化路径格式 - 仅使用正斜杠，目录以斜杠结尾
	 * 
	 * @param init 要标准化的原始路径
	 * @param bIsDir 是否为目录路径，默认为true。如果为true，则确保路径以斜杠结尾
	 * @return std::string 标准化后的路径
	 */
	static inline std::string standardisePath( const std::string &init, bool bIsDir = true)
	{
		std::string path = init;

		std::replace( path.begin(), path.end(), '\\', '/' );
		if (path[path.length() - 1] != '/' && bIsDir)
			path += '/';

		return std::move(path);
	}

	/**
	 * @brief 将完整的文件路径分割为基础文件名和路径
	 * 
	 * @param qualifiedName 完整的文件路径
	 * @param outBasename 输出参数，用于存储提取出的文件名
	 * @param outPath 输出参数，用于存储提取出的路径
	 * @note 路径会按照standardisePath方法的规则进行标准化，使用正斜杠
	 */
	static inline void splitFilename(const std::string& qualifiedName,std::string& outBasename, std::string& outPath)
	{
		std::string path = qualifiedName;
		// Replace \ with / first
		std::replace( path.begin(), path.end(), '\\', '/' );
		// split based on final /
		size_t i = path.find_last_of('/');

		if (i == std::string::npos)
		{
			outPath = "";
			outBasename = qualifiedName;
		}
		else
		{
			outBasename = path.substr(i+1, path.size() - i - 1);
			outPath = path.substr(0, i+1);
		}
	}

	/**
	 * @brief 简单的模式匹配函数，支持通配符
	 * 
	 * @param str 要测试的字符串
	 * @param pattern 要匹配的模式，可以包含简单的'*'通配符
	 * @param caseSensitive 是否区分大小写，默认为true
	 * @return bool 如果字符串与模式匹配则返回true，否则返回false
	 */
	static inline bool match(const std::string& str, const std::string& pattern, bool caseSensitive = true)
	{
		std::string tmpStr = str;
		std::string tmpPattern = pattern;
		if (!caseSensitive)
		{
			toLowerCase(tmpStr);
			toLowerCase(tmpPattern);
		}

		std::string::const_iterator strIt = tmpStr.begin();
		std::string::const_iterator patIt = tmpPattern.begin();
		std::string::const_iterator lastWildCardIt = tmpPattern.end();
		while (strIt != tmpStr.end() && patIt != tmpPattern.end())
		{
			if (*patIt == '*')
			{
				lastWildCardIt = patIt;
				// Skip over looking for next character
				++patIt;
				if (patIt == tmpPattern.end())
				{
					// Skip right to the end since * matches the entire rest of the string
					strIt = tmpStr.end();
				}
				else
				{
					// scan until we find next pattern character
					while(strIt != tmpStr.end() && *strIt != *patIt)
						++strIt;
				}
			}
			else
			{
				if (*patIt != *strIt)
				{
					if (lastWildCardIt != tmpPattern.end())
					{
						// The last wildcard can match this incorrect sequence
						// rewind pattern to wildcard and keep searching
						patIt = lastWildCardIt;
						lastWildCardIt = tmpPattern.end();
					}
					else
					{
						// no wildwards left
						return false;
					}
				}
				else
				{
					++patIt;
					++strIt;
				}
			}

		}
		// If we reached the end of both the pattern and the string, we succeeded
		if (patIt == tmpPattern.end() && strIt == tmpStr.end())
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	/**
	 * @brief 返回一个空的字符串常量
	 * 
	 * @return const std::string 空字符串常量，当需要返回引用而本地变量不存在时非常有用
	 */
	static inline const std::string BLANK()
	{
		static const std::string temp = std::string("");
		return std::move(temp);
	}

	/**
	 * @brief 格式化字符串，类似于 C 语言的 printf 函数
	 * 
	 * @param pszFormat 格式化字符串，包含格式化控制符
	 * @param ... 可变参数列表，对应格式化字符串中的控制符
	 * @return std::string 格式化后的字符串
	 * @note std::string 没有类似 CString 的 Format 函数，因此自行实现
	 */
	static inline std::string printf(const char *pszFormat, ...)
	{
		va_list argptr;
		va_start(argptr, pszFormat);
		std::string result=printf(pszFormat,argptr);
		va_end(argptr);
		return std::move(result);
	}

	/**
	 * @brief 格式化字符串的另一个实现，类似于 C 语言的 printf 函数
	 * 
	 * @param pszFormat 格式化字符串，包含格式化控制符
	 * @param ... 可变参数列表，对应格式化字符串中的控制符
	 * @return std::string 格式化后的字符串
	 * @note 这是另一种格式化字符串的实现方式，提供多样化的格式化选项
	 */
	static inline std::string printf2(const char *pszFormat, ...)
	{
		va_list argptr;
		va_start(argptr, pszFormat);
		std::string result=printf2(pszFormat,argptr);
		va_end(argptr);
		return std::move(result);
	}

	/**
	 * @brief 格式化字符串的底层实现，接受va_list参数
	 * 
	 * @param pszFormat 格式化字符串，包含格式化控制符
	 * @param argptr 已初始化的可变参数列表
	 * @return std::string 格式化后的字符串
	 * @note 这是C语言风格的可变参数字符串格式化的内部实现
	 */
	static inline std::string printf2(const char *pszFormat,va_list argptr)
	{
		int         size   = 1024;
		char*       buffer = new char[size];

		while (1)
		{
#ifdef _MSC_VER
			int n = _vsnprintf(buffer, size, pszFormat, argptr);
#else
			int n = vsnprintf(buffer, size, pszFormat, argptr);
#endif

			// If that worked, return a string.
			if (n > -1 && n < size)
			{
				std::string s(buffer);
				delete [] buffer;
				return s;
			}

			if (n > -1)     size  = n+1; // ISO/IEC 9899:1999
			else            size *= 2;   // twice the old size

			delete [] buffer;
			buffer = new char[size];
		}
	}

	/**
	 * @brief 将字符串通过在两端添加空格扩展到指定长度
	 * 
	 * @param str 要扩展的字符串
	 * @param length 指定的目标长度
	 * @return std::string 扩展后的字符串，如果原字符串长度已经超过指定长度，则返回原字符串
	 * @note 扩展时会尽量平均地在字符串两端添加空格
	 */
	static inline std::string extend(const char* str, uint32_t length)
	{
		if(strlen(str) >= length)
			return str;

		std::string ret = str;
		uint32_t spaces = length - (uint32_t)strlen(str);
		uint32_t former = spaces/2;
		uint32_t after = spaces - former;
		for(uint32_t i = 0; i < former; i++)
		{
			ret.insert(0, " ");
		}

		for(uint32_t i = 0; i < after; i++)
		{
			ret += " ";
		}
		return std::move(ret);
	}

	/**
	 * @brief 格式化字符串的内部实现，接受va_list参数
	 * 
	 * @param pszFormat 格式化字符串，包含格式化控制符
	 * @param argptr 已初始化的可变参数列表
	 * @return std::string 格式化后的字符串
	 * @note 这是第一个 printf 函数的内部实现，实现类似 C 语言格式化字符串的功能
	 */
	static inline std::string printf(const char* pszFormat, va_list argptr)
	{
		int size = 1024;
		int len=0;
		std::string ret;
		for ( ;; )
		{
			ret.resize(size + 1,0);
			char *buf=(char *)ret.c_str();   
			if ( !buf )
			{
				return BLANK();
			}

			va_list argptrcopy;
			va_copy(argptrcopy, argptr);

#ifdef _MSC_VER
			len = _vsnprintf(buf, size, pszFormat, argptrcopy);
#else
			len = vsnprintf(buf, size, pszFormat, argptrcopy);
#endif
			va_end(argptrcopy);

			if ( len >= 0 && len <= size )
			{
				// ok, there was enough space
				break;
			}
			size *= 2;
		}
		ret.resize(len);
		return std::move(ret);
	}

	/**
	 * @brief 获取字符串右边的N个字符
	 * 
	 * @param src 输入字符串
	 * @param nCount 要获取的字符数量
	 * @return std::string 提取的字符串，如果要提取的字符数量大于字符串长度，则返回空字符串
	 */
	static inline std::string right(const std::string &src,size_t nCount)
	{
		if(nCount>src.length())
			return BLANK();
		return std::move(src.substr(src.length()-nCount,nCount));
	}

	/**
	 * @brief 获取字符串左边的N个字符
	 * 
	 * @param src 输入字符串
	 * @param nCount 要获取的字符数量
	 * @return std::string 提取的字符串，如果要提取的字符数量超过字符串长度，则返回原字符串
	 */
	static inline std::string left(const std::string &src,size_t nCount)
	{
		return std::move(src.substr(0,nCount));
	}

	/**
	 * @brief 计算字符串中指定字符的出现次数
	 * 
	 * @param src 要搜索的字符串
	 * @param ch 要计数的字符
	 * @return size_t 指定字符在字符串中的出现次数
	 */
	static inline size_t charCount(const std::string &src,char ch)
	{
		size_t result=0;
		for(size_t i=0;i<src.length();i++)
		{
			if(src[i]==ch)result++;
		}
		return result;
	}

	/**
	 * @brief 替换字符串中的所有指定子字符串
	 * 
	 * @param str 要处理的字符串引用，替换完成后会直接修改原字符串
	 * @param src 要替换的目标子字符串
	 * @param des 替换后的新字符串
	 */
	static inline void replace(std::string& str, const char* src, const char* des)
	{
		std::string ret = "";
		std::size_t srcLen = strlen(src);
		std::size_t lastPos = 0;
		std::size_t pos = str.find(src);
		while(pos != std::string::npos)
		{
			ret += str.substr(lastPos, pos-lastPos);
			ret += des;

			lastPos = pos + srcLen;
			pos = str.find(src, lastPos);
		}
		ret += str.substr(lastPos, pos);

		str = ret;
	}
};
