/*!
 * \file charconv.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 字符编码转换工具类
 * 
 * \details 本文件实现了一组字符编码相关的工具类，包括：
 * - UTF8toChar：将UTF-8编码字符串转换为本地编码（Windows下CP_ACP，Linux下gb2312）
 * - ChartoUTF8：将本地编码字符串转换为UTF-8编码
 * - URLEncode：实现URL编码，将特殊字符转换为%XX形式
 * - URLDecode：实现URL解码，将%XX形式的字符转换回原始字符
 * - EncodingHelper：提供字符编码检测功能，可以检测字符串是否为GBK或UTF-8编码
 * 
 * 这些类在多语言环境下处理中文或其他非英文字符时非常有用，
 * 对跨平台开发提供了编码转换支持。
 */
#pragma once
#include <stdlib.h>
#include <string>
#include <cstring>
#include <iconv.h>
#ifdef _MSC_VER
#include <windows.h>
#else
#include <iconv.h>
#endif

/**
 * @brief UTF-8编码字符串转换为本地编码的工具类
 * 
 * @details 该类用于将UTF-8编码的字符串转换为本地编码：
 * - Windows系统下转换为CP_ACP（系统当前活动代码页）
 * - Linux系统下转换为GB2312编码
 * 
 * 该类实现了自动类型转换运算符，可以直接将类实例当作字符串指针使用。
 * 如果字符串只包含ASCII字符，则不会进行转换，以提高效率。
 * 
 * 类实例会自动管理转换后字符串的内存，在析构时释放内存。
 */
class UTF8toChar
{
public :
	/**
	 * @brief 使用C风格字符串构造UTF8toChar对象
	 * 
	 * @param utf8_string UTF-8编码的字符串指针
	 * 
	 * @details 构造函数将调用init方法完成实际的编码转换工作
	 */
	UTF8toChar(const char *utf8_string)
	{
		init(utf8_string);
	}

	/**
	 * @brief 使用std::string构造UTF8toChar对象
	 * 
	 * @param utf8_string UTF-8编码的std::string字符串
	 * 
	 * @details 该构造函数将std::string转换为C风格字符串，然后调用init方法完成实际的编码转换
	 */
	UTF8toChar(const std::string& utf8_string)
	{
		init(utf8_string.c_str());
	}

	/**
	 * @brief 初始化并执行字符编码转换
	 * 
	 * @param utf8_string UTF-8编码的字符串指针
	 * 
	 * @details 这个方法执行实际的编码转换工作，处理流程如下：
	 * 1. 如果输入字符串为空指针，则输出也设置为空指针
	 * 2. 如果输入字符串为空字符串，则直接使用空字符串常量
	 * 3. 如果输入字符串只包含ASCII字符，则直接使用原字符串指针，不需要转换
	 * 4. 如果字符串包含非ASCII字符，则执行编码转换：
	 *    - Windows下：先将UTF-8转换成Unicode，再将Unicode转换为CP_ACP
	 *    - Linux下：使用iconv将UTF-8直接转换为GB2312
	 * 
	 * 当需要转换时，会动态分配内存并设置needFree标志，以便在析构时释放内存。
	 */
	void init(const char *utf8_string)
	{
		if (0 == utf8_string)
			t_string = 0;
		else if (0 == *utf8_string)
		{
			needFree = false;
			t_string = (char *)("");
		}
		else if ( isPureAscii(utf8_string))
		{
			needFree = false;
			t_string = (char *)utf8_string;
		}
		else
		{
			// Either TCHAR = Unicode (2 bytes), or utf8_string contains non-ASCII characters.
			// Needs conversion
			needFree = true;

			// Convert to Unicode (2 bytes)
			std::size_t string_len = strlen(utf8_string);
			std::size_t dst_len = string_len * 2 + 2;
#ifdef _MSC_VER
			wchar_t *buffer = new wchar_t[string_len + 1];
			MultiByteToWideChar(CP_UTF8, 0, utf8_string, -1, buffer, (int)string_len + 1);
			buffer[string_len] = 0;

			t_string = new char[string_len * 2 + 2];
			WideCharToMultiByte(CP_ACP, 0, buffer, -1, t_string, (int)dst_len, 0, 0);
			t_string[string_len * 2 + 1] = 0;
			delete[] buffer;
#else
			iconv_t cd;
			t_string = new char[dst_len];
			char* p = t_string;
			cd = iconv_open("gb2312", "utf-8");
			if (cd != 0)
			{
				memset(t_string, 0, dst_len);
				iconv(cd, (char**)&utf8_string, &string_len, &p, &dst_len);
				iconv_close(cd);
				t_string[dst_len] = '\0';
			}
#endif
		}
	}

	/**
	 * @brief 类型转换运算符，将对象转换为字符串指针
	 * 
	 * @return const char* 转换后的本地编码字符串指针
	 * 
	 * @details 该运算符允许将UTF8toChar对象直接用于需要const char*类型的表达式中
	 * 例如：printf("%s", UTF8toChar(utf8_str));
	 */
	operator const char*()
	{
		return t_string;
	}

	/**
	 * @brief 获取转换后的C风格字符串
	 * 
	 * @return const char* 转换后的本地编码字符串指针
	 * 
	 * @details 该方法与类型转换运算符功能相同，返回转换后的字符串指针。
	 * 提供该方法是为了与C++标准库容器类的c_str()方法保持一致的命名风格。
	 */
	const char* c_str()
	{
		return t_string;
	}

	/**
	 * @brief 析构函数
	 * 
	 * @details 析构函数负责释放在转换过程中可能分配的内存。
	 * 只有当needFree标志为true时（即实际进行了字符编码转换并分配了新内存），
	 * 才会释放t_string指向的内存。如果是直接使用原字符串指针（ASCII字符串或空字符串），
	 * 则不需要释放内存。
	 */
	~UTF8toChar()
	{
		if (needFree)
			delete[] t_string;
	}

private :
	/**
	 * @brief 转换后的本地编码字符串指针
	 * 
	 * @details 存储转换后的本地编码字符串指针（Windows下CP_ACP，Linux下gb2312）。
	 * 根据输入字符串的类型，可能指向原始字符串（ASCII字符）或新分配的内存（非ASCII字符）。
	 */
	char *t_string;

	/**
	 * @brief 标记是否需要释放字符串内存
	 * 
	 * @details 当为true时，表示转换过程中分配了新内存，需要在析构函数中释放内存。
	 * 当为false时，表示t_string指向原始字符串或字符串常量，不需要释放内存。
	 */
	bool needFree;

	//
	// helper utility to test if a string contains only ASCII characters
	//
	bool isPureAscii(const char *s)
	{
		while (*s != 0) { if (*(s++) & 0x80) return false; }
		return true;
	}

	//disable assignment
	UTF8toChar(const UTF8toChar &rhs);
	UTF8toChar &operator=(const UTF8toChar &rhs);
};

/**
 * @brief 本地编码字符串转换为UTF-8编码的工具类
 * 
 * @details 该类用于将本地编码的字符串转换为UTF-8编码：
 * - Windows系统下从系统当前活动代码页（CP_ACP）转换为UTF-8
 * - Linux系统下从本地编码（通常是gb2312）转换为UTF-8
 * 
 * 该类是UTF8toChar类的逆操作，实现了自动类型转换运算符以及内存管理功能。
 * 如果字符串只包含ASCII字符，则不会进行转换，以提高效率。
 */
class ChartoUTF8
{
public :
	/**
	 * @brief 使用std::string构造ChartoUTF8对象
	 * 
	 * @param str 本地编码的std::string字符串
	 * 
	 * @details 该构造函数将std::string转换为C风格字符串，然后调用init方法完成实际的编码转换
	 */
	ChartoUTF8(const std::string& str)
	{
		init(str.c_str());
	}

	/**
	 * @brief 使用C风格字符串构造ChartoUTF8对象
	 * 
	 * @param t_string 本地编码的字符串指针
	 * 
	 * @details 构造函数将调用init方法完成实际的编码转换工作
	 */
	ChartoUTF8(const char *t_string)
	{
		init(t_string);
	}

	/**
	 * @brief 初始化并执行字符编码转换
	 * 
	 * @param t_string 本地编码的字符串指针
	 * 
	 * @details 这个方法执行实际的编码转换工作，将本地编码转换为UTF-8编码。处理流程如下：
	 * 1. 如果输入字符串为空指针，则输出也设置为空指针
	 * 2. 如果输入字符串为空字符串，则直接使用空字符串常量
	 * 3. 如果输入字符串只包含ASCII字符，则直接使用原字符串指针，不需要转换
	 * 4. 如果字符串包含非ASCII字符，则执行编码转换：
	 *    - Windows下：先将本地编码转换成Unicode，再将Unicode转换为UTF-8
	 *    - Linux下：使用iconv将gb2312直接转换为UTF-8
	 * 
	 * 当需要转换时，会动态分配内存并设置needFree标志，以便在析构时释放内存。
	 * 注意在Linux下要求输入的字符串必须是gb2312编码。
	 */
	void init(const char *t_string)
	{
		if (0 == t_string)
			utf8_string = 0;
		else if (0 == *t_string)
		{
			utf8_string = (char *)("");
			needFree = false;
		}
		else if (isPureAscii((char *)t_string))
		{
			utf8_string = (char *)t_string;
			needFree = false;
		}
		else
		{

			needFree = true;

			std::size_t string_len = strlen(t_string);
			std::size_t dst_len = string_len * 5;
#ifdef _MSC_VER		

			// Convert to Unicode if not already in unicode.
			wchar_t *w_string = new wchar_t[string_len + 1];
			MultiByteToWideChar(CP_ACP, 0, t_string, -1, w_string, (int)string_len + 1);
			w_string[string_len] = 0;

			// Convert from Unicode (2 bytes) to UTF8
			utf8_string = new char[dst_len];
			WideCharToMultiByte(CP_UTF8, 0, w_string, -1, utf8_string, (int)dst_len, 0, 0);
			utf8_string[string_len * 3] = 0;

			if (w_string != (wchar_t *)t_string)
				delete[] w_string;
#else
			iconv_t cd;
			utf8_string = new char[dst_len];
			char* p = utf8_string;
			cd = iconv_open("utf-8", "gb2312");
			if (cd != 0)
			{
				memset(utf8_string, 0, dst_len);
				iconv(cd, (char**)&t_string, &string_len, &p, &dst_len);
				iconv_close(cd);
			}
#endif
		}
	}

	/**
	 * @brief 类型转换运算符，将对象转换为字符串指针
	 * 
	 * @return const char* 转换后的UTF-8编码字符串指针
	 * 
	 * @details 该运算符允许将ChartoUTF8对象直接用于需要const char*类型的表达式中
	 * 例如：printf("%s", ChartoUTF8(str));
	 */
	operator const char*()
	{
		return utf8_string;
	}

	/**
	 * @brief 获取转换后的C风格字符串
	 * 
	 * @return const char* 转换后的UTF-8编码字符串指针
	 * 
	 * @details 该方法与类型转换运算符功能相同，返回转换后的字符串指针。
	 * 提供该方法是为了与C++标准库容器类的c_str()方法保持一致的命名风格。
	 */
	const char* c_str() const
	{
		return utf8_string;
	}

	/**
	 * @brief 析构函数
	 * 
	 * @details 析构函数负责释放在转换过程中可能分配的内存。
	 * 只有当needFree标志为true时（即实际进行了字符编码转换并分配了新内存），
	 * 才会释放utf8_string指向的内存。如果是直接使用原字符串指针（ASCII字符串或空字符串），
	 * 则不需要释放内存。
	 */
	~ChartoUTF8()
	{
		if (needFree)
			delete[] utf8_string;
	}

private :
	/**
	 * @brief 转换后的UTF-8编码字符串指针
	 * 
	 * @details 存储转换后的UTF-8编码字符串指针。
	 * 根据输入字符串的类型，可能指向原始字符串（ASCII字符）或新分配的内存（非ASCII字符）。
	 */
	char *utf8_string;

	/**
	 * @brief 标记是否需要释放字符串内存
	 * 
	 * @details 当为true时，表示转换过程中分配了新内存，需要在析构函数中释放内存。
	 * 当为false时，表示utf8_string指向原始字符串或字符串常量，不需要释放内存。
	 */
	bool needFree;

	//
	// helper utility to test if a string contains only ASCII characters
	//
	bool isPureAscii(const char *s)
	{
		while (*s != 0) { if (*(s++) & 0x80) return false; }
		return true;
	}

	//disable assignment
	ChartoUTF8(const ChartoUTF8 &rhs);
	ChartoUTF8 &operator=(const ChartoUTF8 &rhs);
};


/**
 * @brief URL编码工具类
 * 
 * @details 该类用于将字符串转换为URL编码格式，将非ASCII字符和一些特殊字符（如空格）
 * 转换为%XX格式，其中XX是字符的十六进制编码值。
 * 
 * 这在构建URL字符串时非常有用，特别是当URL包含中文或其他非英文字符时。
 * 类实例可以通过类型转换运算符直接转换为字符串指针使用。
 */
class URLEncode
{
public:
	/**
	 * @brief 使用字符串构造URL编码对象
	 * 
	 * @param src 需要进行URL编码的原始字符串
	 * 
	 * @details 构造函数自动对输入字符串进行URL编码处理，处理规则如下：
	 * 1. 空格字符被转换为"%20"
	 * 2. ASCII字符（非空格）保持不变
	 * 3. 非ASCII字符被转换为"%XX"格式，其中XX是字符的十六进制编码值
	 * 
	 * 编码结果存储在encoded_string成员变量中，可以通过类型转换运算符或c_str()方法访问。
	 */
	URLEncode(const char* src)
	{ 
		char hex[] = "0123456789ABCDEF";  

		for (unsigned int i = 0; i < strlen(src); ++i)
		{  
			const char cc = src[i];  
			if (isPureAscii(&cc))  
			{  
				if (cc == ' ')  
				{  
					encoded_string += "%20";  
				}  
				else 
					encoded_string += cc;  
			}  
			else 
			{  
				unsigned char c = static_cast<unsigned char>(src[i]);
				encoded_string += '%';  
				encoded_string += hex[c / 16];  
				encoded_string += hex[c % 16];
			}  
		}  
	}
	
	/**
	 * @brief 类型转换运算符，将对象转换为字符串指针
	 * 
	 * @return const char* 编码后的URL字符串指针
	 * 
	 * @details 该运算符允许将URLEncode对象直接用于需要const char*类型的表达式中
	 * 例如：printf("%s", URLEncode(str));
	 */
	operator const char*(){return encoded_string.c_str();}

private:
	/**
	 * @brief 检测字符串是否只包含ASCII字符
	 * 
	 * @param s 要检测的字符串指针
	 * @return bool 如果字符串只包含ASCII字符返回true，否则返回false
	 * 
	 * @details 该方法用于检测字符串是否只包含ASCII字符（即第一位不为1的字符，编码值小于128）。
	 * 方法逐个字符检查，如果字符的最高位（0x80位）为1，则表示该字符是一个非ASCII字符，返回false。
	 * 如果整个字符串中没有发现非ASCII字符，则返回true。
	 */
	bool isPureAscii(const char *s)
	{
		while (*s != 0) { if (*(s++) & 0x80) return false; }
		return true;
	}

private:
	std::string encoded_string;
};

/**
 * @brief URL解码工具类
 * 
 * @details 该类用于将URL编码格式的字符串转换回原始字符串，将“%XX”格式的十六进制编码和“+”符号
 * 转换回对应的原始字符。
 * 
 * 这在解析URL参数或表单数据时非常有用，可以正确处理包含中文或其他非英文字符的URL。
 * 类实例可以通过类型转换运算符直接转换为字符串指针使用。
 */
class URLDecode
{
public:
	/**
	 * @brief 使用URL编码字符串构造URL解码对象
	 * 
	 * @param src 需要解码的URL编码字符串
	 * 
	 * @details 构造函数自动对输入的URL编码字符串进行解码处理，处理规则如下：
	 * 1. "+"符号被转换为空格字符
	 * 2. "%XX"形式的十六进制编码被转换为对应的字符，其中XX是十六进制值
	 * 3. 对于常见的URL合法字符（字母、数字、特定特殊符号），即使以"%XX"格式编码也保持原样，
	 *    这些字符包括[0-9a-zA-Z][$-_.+!*'(),][$&+,/:;=?@]
	 * 4. 其他字符保持原样
	 * 
	 * 解码结果存储在decoded_string成员变量中，可以通过类型转换运算符访问。
	 */
	URLDecode(const char* src)
	{ 
		int hex = 0;  
		for (unsigned int i = 0; i < strlen(src); ++i)
		{  
			switch (src[i])  
			{  
			case '+':  
				decoded_string += ' ';  
				break;  
			case '%':  
				if (isxdigit(src[i + 1]) && isxdigit(src[i + 2]))  
				{
					std::string hexStr;
					hexStr += src[i+1];
					hexStr += src[i+2];
					hex = strtol(hexStr.c_str(), 0, 16);
					//字母和数字[0-9a-zA-Z]、一些特殊符号[$-_.+!*'(),] 、以及某些保留字[$&+,/:;=?@]  
					//可以不经过编码直接用于URL  
					if (!((hex >= 48 && hex <= 57) || //0-9  
						(hex >=97 && hex <= 122) ||   //a-z  
						(hex >=65 && hex <= 90) ||    //A-Z  
						//一些特殊符号及保留字[$-_.+!*'(),]  [$&+,/:;=?@]  
						hex == 0x21 || hex == 0x24 || hex == 0x26 || hex == 0x27 || hex == 0x28 || hex == 0x29 
						|| hex == 0x2a || hex == 0x2b|| hex == 0x2c || hex == 0x2d || hex == 0x2e || hex == 0x2f 
						|| hex == 0x3A || hex == 0x3B|| hex == 0x3D || hex == 0x3f || hex == 0x40 || hex == 0x5f 
						))  
					{  
						decoded_string += char(hex);
						i += 2;  
					}  
					else decoded_string += '%';  
				}else {  
					decoded_string += '%';  
				}  
				break;  
			default:
				decoded_string += src[i];  
				break;  
			}  
		}  
	}

	operator const char*(){return decoded_string.c_str();}

private:
	std::string decoded_string;
};

/**
 * @brief 字符编码检测帮助类
 * 
 * @details 该类提供了字符编码检测功能，可以检测一个字符串是否为GBK编码或UTF-8编码。
 * 所有方法都是静态的，可以直接通过类名调用，不需要创建类实例。
 * 
 * 这在处理来自不同源的文本数据时非常有用，可以在转换之前先确定字符串的编码格式。
 */
class EncodingHelper
{
public:
	/**
	 * @brief 检测数据是否为GBK编码
	 * 
	 * @param data 要检测的数据的指针
	 * @param len 数据长度
	 * @return bool 如果数据包含GBK编码返回true，否则返回false
	 * 
	 * @details 该方法通过检查字节序列是否符合GBK编码规范来判断数据是否为GBK编码。
	 * GBK编码规则如下：
	 * 1. 0x00~0x7F的字符是单字节编码，与ASCII兼容
	 * 2. 0x81~0xFE范围内的字节开头，后跟0x40~0xFE范围内的字节（但不能是0xF7）组成双字节编码
	 * 
	 * 只要发现符合GBK编码规则的双字节序列，就认为整个字符串是GBK编码。
	 */
	static bool isGBK(unsigned char* data, std::size_t len) {
		std::size_t i = 0;
		while (i < len) {
			if (data[i] <= 0x7f) {
				//编码小于等于127,只有一个字节的编码，兼容ASCII
				i++;
				continue;
			}
			else {
				//大于127的使用双字节编码
				if (data[i] >= 0x81 &&
					data[i] <= 0xfe &&
					data[i + 1] >= 0x40 &&
					data[i + 1] <= 0xfe &&
					data[i + 1] != 0xf7) 
				{
					//如果有GBK编码的，就算整个字符串都是GBK编码
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * @brief 计算UTF-8编码字节首部连续为1的比特数量
	 * 
	 * @param byte 要检测的字节
	 * @return int 字节首部连续1的比特数量
	 * 
	 * @details 该方法用于判断UTF-8编码中一个字符所使用的字节数。
	 * 在UTF-8编码中，首字节的表现形式如下：
	 * - 0xxxxxxx: 单字节字符（ASCII字符），返回0
	 * - 110xxxxx: 双字节字符，返回2
	 * - 1110xxxx: 三字节字符，返回3
	 * - 11110xxx: 四字节字符，返回4
	 * - 111110xx: 五字节字符，返回5
	 * - 1111110x: 六字节字符，返回6
	 * 
	 * 这个数字表示在UTF-8编码中该字符占用的字节数。
	 */
	static int preNUm(unsigned char byte) {
		unsigned char mask = 0x80;
		int num = 0;
		for (int i = 0; i < 8; i++) {
			if ((byte & mask) == mask) {
				mask = mask >> 1;
				num++;
			}
			else {
				break;
			}
		}
		return num;
	}


	static bool isUtf8(unsigned char* data, std::size_t len) {
		int num = 0;
		std::size_t i = 0;
		while (i < len) {
			if ((data[i] & 0x80) == 0x00) 
			{
				// 0XXX_XXXX
				i++;
				continue;
			}
			else if ((num = preNUm(data[i])) > 2) 
			{
				// 110X_XXXX 10XX_XXXX
				// 1110_XXXX 10XX_XXXX 10XX_XXXX
				// 1111_0XXX 10XX_XXXX 10XX_XXXX 10XX_XXXX
				// 1111_10XX 10XX_XXXX 10XX_XXXX 10XX_XXXX 10XX_XXXX
				// 1111_110X 10XX_XXXX 10XX_XXXX 10XX_XXXX 10XX_XXXX 10XX_XXXX
				// preNUm() 返回首个字节8个bits中首bit前面1bit的个数，该数量也是该字符所使用的字节数        
				i++;
				for (int j = 0; j < num - 1; j++) {
					//判断后面num - 1 个字节是不是都是10开
					if ((data[i] & 0xc0) != 0x80) {
						return false;
					}
					i++;
				}
			}
			else 
			{
				//其他情况说明不是utf-8
				return false;
			}
		}
		return true;
	}
};