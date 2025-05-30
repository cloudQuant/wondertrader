/**
 * @file cppcli.hpp
 * @brief 命令行参数解析工具库
 * 
 * @details 该文件提供了一个轻量级的命令行参数解析库，用于处理程序启动时的命令行参数。
 * 主要功能包括：
 * - 命令行参数的解析和存储
 * - 参数的类型检查和验证
 * - 必选参数的验证
 * - 参数值范围的验证
 * - 帮助文档的自动生成
 * - 路径处理工具
 * 
 * 使用该库可以简化命令行参数的处理流程，提高程序的健壮性和用户体验。
 */

#pragma once

#include <algorithm>
#include <assert.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <vector>

/**
 * @brief 平台相关定义和包含文件
 * 
 * @details 根据不同的操作系统平台，包含相应的头文件并定义路径分隔符
 */
#if defined(WIN32) || defined(_WIN64) || defined(__WIN32__)
    #include "direct.h"
    #include <windows.h>
    /**
     * @brief Windows平台下的路径分隔符
     */
    #define CPPCLI_SEPARATOR_TYPE    "\\"
    /**
     * @brief Windows平台下非本平台的路径分隔符，用于路径转换
     */
    #define CPPCLI_SEPARATOR_NO_TYPE "/"
#else
    #include <unistd.h>
    /**
     * @brief Unix/Linux平台下的路径分隔符
     */
    #define CPPCLI_SEPARATOR_TYPE    "/"
    /**
     * @brief Unix/Linux平台下非本平台的路径分隔符，用于路径转换
     */
    #define CPPCLI_SEPARATOR_NO_TYPE "\\"
#endif

// #define CPPCLI_DEBUG

/**
 * @namespace cppcli
 * @brief 命令行参数解析库的主命名空间
 * 
 * @details 该命名空间包含了命令行参数解析库的所有类、函数和工具
 * 主要组件包括 Option 类和 Rule 类，以及各种辅助工具和枚举类型
 */
namespace cppcli {
    /**
     * @brief 用于同步控制台输出的互斥锁
     * 
     * @details 在多线程环境下保证控制台输出的原子性
     */
    static std::mutex _coutMutex;

#ifdef CPPCLI_DEBUG
    /**
     * @brief 调试信息打印函数
     * 
     * @tparam Args 可变参数类型
     * @param args 要打印的参数列表
     * 
     * @details 该函数在定义了CPPCLI_DEBUG宏的情况下可用，用于输出调试信息。
     * 使用互斥锁保证多线程环境下的输出安全，并在每条调试信息前添加标记。
     * 使用可变参数模板支持打印多种类型的参数。
     */
    template <class... Args>
    void __cppcli_debug_print(const Args &...args)
    {
        std::unique_lock<std::mutex> lock(_coutMutex);
        std::cout << "[CPPCLI_DEBUG] ";
        auto printFunc = [](auto i) { std::cout << i; };
        std::initializer_list<int>{(printFunc(args), 0)...};
        std::cout << std::endl;
    }

#endif
}   // namespace cppcli

#ifdef CPPCLI_DEBUG
    #define CPPCLI_DEBUG_PRINT(...) cppcli::__cppcli_debug_print(__VA_ARGS__)
#endif

namespace cppcli {
    class Option;
    class Rule;

    /**
     * @enum ErrorExitEnum
     * @brief 错误退出类型枚举
     * 
     * @details 定义在遇到错误时程序应该如何退出
     */
    enum ErrorExitEnum {
        EXIT_PRINT_RULE = 0x00,          /**< 只打印规则信息然后退出 */
        EXIT_PRINT_RULE_HELPDOC = 0x01, /**< 打印规则信息和帮助文档然后退出 */
    };
    
    /**
     * @enum HelpDocEnum
     * @brief 帮助文档类型枚举
     * 
     * @details 定义使用哪种类型的帮助文档
     */
    enum HelpDocEnum {
        USE_DEFAULT_HELPDOC = 0x00,      /**< 使用默认的帮助文档 */
        USE_UER_DEFINED_HELPDOC = 0x01, /**< 使用用户自定义的帮助文档 */
    };

    /**
     * @namespace detail
     * @brief 内部实现细节的命名空间
     * 
     * @details 包含了cppcli库的内部实现细节，如工具类、枚举类型等
     */
    namespace detail {
        /**
         * @enum ErrorEventType
         * @brief 错误事件类型枚举
         * 
         * @details 定义了可能发生的不同类型的错误事件
         */
        enum ErrorEventType {
            MECESSARY_ERROR = 0x00, /**< 必选参数缺失错误 */
            VALUETYPE_ERROR = 0x01, /**< 参数值类型错误 */
            ONEOF_ERROR = 0x02,     /**< 参数值不在指定选项中的错误 */
            NUMRANGE_ERROR = 0x03,  /**< 参数值超出指定范围的错误 */
        };

        /**
         * @enum ValueTypeEnum
         * @brief 参数值类型枚举
         * 
         * @details 定义了命令行参数可能的值类型
         */
        enum ValueTypeEnum {
            STRING = 0x00, /**< 字符串类型 */
            INT = 0x01,    /**< 整数类型 */
            DOUBLE = 0x02, /**< 浮点数类型 */
        };

        /**
         * @class pathUtil
         * @brief 路径处理工具类
         * 
         * @details 提供了一系列静态方法来处理文件路径，如提取文件名、后缀名、目录等
         */
        class pathUtil final {
          private:
            /**
             * @brief 替换字符串中所有指定模式的子串
             * 
             * @param str 要处理的字符串
             * @param pattern 要替换的模式
             * @param newpat 新的替换内容
             * @return int 替换的次数
             * 
             * @details 在字符串中查找所有指定模式的子串，并替换为新的内容
             */
            static int replace_all(std::string &str, const std::string &pattern, const std::string &newpat);

          public:
            /**
             * @brief 从路径中获取文件名
             * 
             * @param filePath 文件路径
             * @return std::string 文件名（包含后缀）
             * 
             * @details 从完整路径中提取文件名部分，包含文件后缀
             */
            static std::string getFilename(const std::string &filePath);
            
            /**
             * @brief 从路径中获取没有后缀的文件名
             * 
             * @param filePath 文件路径
             * @return std::string 没有后缀的文件名
             * 
             * @details 从完整路径中提取文件名部分，不包含文件后缀
             */
            static std::string getFilenameWithOutSuffix(const std::string &filePath);
            
            /**
             * @brief 从路径中获取文件后缀
             * 
             * @param filePath 文件路径
             * @return std::string 文件后缀（不包含点）
             * 
             * @details 从完整路径中提取文件后缀部分，不包含点
             */
            static std::string getFileSuffix(const std::string &filePath);
            
            /**
             * @brief 从路径中获取文件所在目录
             * 
             * @param filePath 文件路径
             * @return std::string 文件所在目录
             * 
             * @details 从完整路径中提取文件所在的目录部分
             */
            static std::string getFileDir(const std::string &filePath);
        };
        /**
         * @class algoUtil
         * @brief 算法工具类
         * 
         * @details 提供了一系列静态方法来处理命令行参数和进行类型验证
         */
        class algoUtil final {
          private:
          public:
            /**
             * @brief 初始化命令行参数映射
             * 
             * @param length 参数数组长度
             * @param strArr 参数数组
             * @param stringMap 输出参数映射
             * 
             * @details 将命令行参数解析并存储到映射中，键为参数名，值为参数值
             */
            static void InitCommandMap(int length, char *strArr[], std::map<std::string, std::string> &stringMap);
            
            /**
             * @brief 检查字符串是否为整数
             * 
             * @param value 要检查的字符串
             * @return bool 如果是整数返回true，否则返回false
             * 
             * @details 检查给定字符串是否可以表示一个有效的整数
             */
            static bool isInt(const std::string &value);
            
            /**
             * @brief 检查字符串是否为浮点数
             * 
             * @param value 要检查的字符串
             * @return bool 如果是浮点数返回true，否则返回false
             * 
             * @details 检查给定字符串是否可以表示一个有效的浮点数
             */
            static bool isDouble(const std::string &value);
            
            /**
             * @brief 验证字符串是否为数字（整数或浮点数）
             * 
             * @param value 要验证的字符串
             * @return bool 如果是数字返回true，否则返回false
             * 
             * @details 检查给定字符串是否可以表示一个有效的数字（整数或浮点数）
             */
            static bool verifyDouble(const std::string &value);
        };


    }   // namespace detail
}   // namespace cppcli

int cppcli::detail::pathUtil::replace_all(std::string &str, const std::string &pattern, const std::string &newpat)
{
    // 记录替换次数
    int count = 0;
    // 新模式和原模式的长度
    const size_t nsize = newpat.size();
    const size_t psize = pattern.size();

    // 查找模式的第一次出现
    size_t pos = str.find(pattern);
    while (pos != std::string::npos) {
        // 替换当前位置的模式
        str.replace(pos, psize, newpat);
        // 从替换后的位置继续查找
        pos = str.find(pattern, pos + nsize);
        // 替换次数增加
        ++count;
    }

    return count;
}

/**
 * @brief 从路径中获取文件名
 * 
 * @param filePath 文件路径
 * @return std::string 文件名（包含后缀）
 * 
 * @details 从完整路径中提取文件名部分，包含文件后缀。
 * 首先将路径分隔符标准化，然后查找最后一个路径分隔符后的内容。
 * 使用assert确保文件存在。
 */
std::string cppcli::detail::pathUtil::getFilename(const std::string &filePath)
{
    // 创建路径的副本
    std::string filePathCopy(filePath);
    // 标准化路径分隔符
    replace_all(filePathCopy, CPPCLI_SEPARATOR_NO_TYPE, CPPCLI_SEPARATOR_TYPE);

    // 确保文件存在
    assert(std::ifstream(filePathCopy.c_str()).good());
    // 查找最后一个路径分隔符的位置，并从其后一个字符开始提取
    std::string::size_type pos = filePathCopy.find_last_of(CPPCLI_SEPARATOR_TYPE) + 1;
    return std::move(filePathCopy.substr(pos, filePathCopy.length() - pos));
}

/**
 * @brief 从路径中获取没有后缀的文件名
 * 
 * @param filePath 文件路径
 * @return std::string 没有后缀的文件名
 * 
 * @details 从完整路径中提取文件名部分，不包含文件后缀。
 * 首先使用getFilename获取完整文件名，然后去除最后一个点及其后的内容。
 */
std::string cppcli::detail::pathUtil::getFilenameWithOutSuffix(const std::string &filePath)
{
    // 获取完整文件名
    std::string filename = getFilename(filePath);
    // 提取从开头到最后一个点之前的部分
    return std::move(filename.substr(0, filename.rfind(".")));
}

/**
 * @brief 从路径中获取文件后缀
 * 
 * @param filePath 文件路径
 * @return std::string 文件后缀（不包含点）
 * 
 * @details 从完整路径中提取文件后缀部分，不包含点。
 * 首先使用getFilename获取完整文件名，然后提取最后一个点后的内容。
 */
std::string cppcli::detail::pathUtil::getFileSuffix(const std::string &filePath)
{
    // 获取完整文件名
    std::string filename = getFilename(filePath);
    // 提取最后一个点后的内容
    return std::move(filename.substr(filename.find_last_of('.') + 1));
}

/**
 * @brief 从路径中获取文件所在目录
 * 
 * @param filePath 文件路径
 * @return std::string 文件所在目录
 * 
 * @details 从完整路径中提取文件所在的目录部分。
 * 首先将路径分隔符标准化，然后提取最后一个路径分隔符前的内容。
 */
std::string cppcli::detail::pathUtil::getFileDir(const std::string &filePath)
{
    // 创建路径的副本
    std::string filePathCopy(filePath);
    // 标准化路径分隔符
    replace_all(filePathCopy, CPPCLI_SEPARATOR_NO_TYPE, CPPCLI_SEPARATOR_TYPE);
    // 查找最后一个路径分隔符的位置
    std::string::size_type pos = filePathCopy.find_last_of(CPPCLI_SEPARATOR_TYPE);
    // 提取从开头到最后一个路径分隔符的部分
    return std::move(filePathCopy.substr(0, pos));
}



/**
 * @brief 初始化命令行参数映射
 * 
 * @param length 参数数组长度
 * @param strArr 参数数组
 * @param stringMap 输出参数映射
 * 
 * @details 将命令行参数解析并存储到映射中，键为参数名，值为参数值。
 * 参数名以短横线开头，如"-h"或"--help"。
 * 如果参数后面没有值或者后面跟的是另一个参数，则该参数的值为空字符串。
 */
void cppcli::detail::algoUtil::InitCommandMap(int length, char *strArr[], std::map<std::string, std::string> &stringMap)
{
    // 初始化临时变量
    std::string keyTmp;      // 当前参数名
    std::string valueTmp;    // 当前参数值
    int keyIndex = -1;       // 当前参数名的索引

    // 遍历命令行参数，从1开始（跳过程序名）
    for (int currentIndex = 1; currentIndex < length; currentIndex++)
    {
        std::string theStr(strArr[currentIndex]);
        // 如果已找到参数名，并且当前字符串是其后的第一个参数
        if (keyIndex != -1 && theStr.size() > 0 && currentIndex == keyIndex + 1)
        {
            // 如果当前字符串是另一个参数名，则上一个参数的值为空
            if(theStr.find_first_of('-') == 0 && theStr.size() > 1 && !isdigit(theStr.at(1)))
            {
                valueTmp = "";
            }
            else
            {   
                // 否则将当前字符串作为上一个参数的值
                valueTmp = theStr;
            }

            // 将参数名和值添加到映射中
            stringMap.insert(std::make_pair(std::move(keyTmp), std::move(valueTmp)));
            keyTmp.clear();
            valueTmp.clear();

            // 重置参数名索引
            keyIndex = -1;
        }

        // 检查当前字符串是否是参数名（以短横线开头，且不是负数）
        if (theStr.find_first_of('-') == 0 && int(std::count(theStr.begin(), theStr.end(), '-')) < theStr.size() && !isdigit(theStr.at(1)))
        {
            keyIndex = currentIndex;
            keyTmp = std::move(theStr);
        }

        // 如果是最后一个参数且是参数名，则其值为空
        if (currentIndex == length - 1 && keyIndex != -1)
        {
            stringMap.insert(std::make_pair(std::move(keyTmp), std::move("")));
        }
    }
}
/**
 * @brief 检查字符串是否为整数
 * 
 * @param value 要检查的字符串
 * @return bool 如果是整数返回true，否则返回false
 * 
 * @details 检查给定字符串是否可以表示一个有效的整数。
 * 整数可以是负数（以短横线开头），其余字符必须全部是数字。
 */
bool cppcli::detail::algoUtil::isInt(const std::string &value)
{
    // 空字符串不是整数
    if (value.empty())
    {
        return false;
    }

    // 如果第一个字符是负号，从第二个字符开始检查
    int startPos = value.at(0) == '-' ? 1 : 0;
    for (int i = startPos; i < value.size(); i++)
    {
        // 如果有非数字字符，则不是整数
        if (isdigit(value.at(i)) == 0)
            return false;
    }
    return true;
}

/**
 * @brief 检查字符串是否为浮点数
 * 
 * @param value 要检查的字符串
 * @return bool 如果是浮点数返回true，否则返回false
 * 
 * @details 检查给定字符串是否可以表示一个有效的浮点数。
 * 浮点数必须包含一个小数点，并且小数点前后都有数字。
 * 浮点数可以是负数（以短横线开头）。
 */
bool cppcli::detail::algoUtil::isDouble(const std::string &value)
{
    // 空字符串不是浮点数
    if (value.empty())
    {
        return false;
    }
    // 浮点数至少需要三个字符（一个数字、小数点、一个数字）
    if (value.size() < 3)
        return false;
    // 处理负数情况
    std::string tmpValue = value.at(0) == '-' ? value.substr(0, value.npos): value;
    // 计算数字的数量
    int numCount = 0;
    for (char const &c : tmpValue)
    {
        if (isdigit(c) != 0)
            numCount++;
    }

    // 判断是否为浮点数：
    // 1. 数字数量比字符串长度少一（小数点）
    // 2. 小数点位置在中间（不在开头或结尾）
    if (numCount == tmpValue.size() - 1 && tmpValue.rfind('.') > 0 && tmpValue.rfind('.') < tmpValue.size() - 1)
    {
        return true;
    }
    return false;
}

/**
 * @brief 验证字符串是否为数字（整数或浮点数）
 * 
 * @param value 要验证的字符串
 * @return bool 如果是数字返回true，否则返回false
 * 
 * @details 检查给定字符串是否可以表示一个有效的数字（整数或浮点数）。
 * 调用isInt和isDouble方法进行验证。
 */
bool cppcli::detail::algoUtil::verifyDouble(const std::string &value)
{
    // 如果是整数或浮点数则返回true
    if (isInt(value) || isDouble(value))
        return true;
    return false;
}

/* ################################################################### */
/* ################################################################### */
/* ################################################################### */
/* ################################################################### */
/* ################################################################### */

namespace cppcli {

    /**
     * @class Rule
     * @brief 命令行参数规则类
     * 
     * @details 该类表示一条命令行参数的规则，包含参数的名称、类型、限制条件、默认值等信息。
     * 可以通过链式调用来设置各种限制条件，如类型限制、取值范围、可选值列表等。
     */
    class Rule {
      private:
        /**
         * @class detail
         * @brief Rule类的内部详细实现
         */
        class detail {
          public:
            /**
             * @struct HelpDocStruct
             * @brief 帮助文档结构
             * 
             * @details 存储帮助文档类型和对应的规则对象
             */
            struct HelpDocStruct {
                static cppcli::HelpDocEnum _helpDocType; /**< 帮助文档类型 */
                static cppcli::Rule *rule;               /**< 帮助参数对应的规则 */
            };
        };

      private:
        friend class Option; /**< 允许Option类访问私有成员 */

        std::string _inputValue;   /**< 用户输入的参数值 */
        std::string _shortParam;   /**< 参数的短名称，如"-h" */
        std::string _longParam;    /**< 参数的长名称，如"--help" */
        std::string _helpInfo;     /**< 参数的帮助信息 */
        bool _necessary = false;   /**< 参数是否必需 */
        std::vector<std::string> _limitOneVec; /**< 参数的可选值列表 */
        std::pair<double, double> _limitNumRange; /**< 数字参数的取值范围 */
        cppcli::detail::ValueTypeEnum _valueType = cppcli::detail::ValueTypeEnum::STRING; /**< 参数值类型 */
        std::string _default = "[EMPTY]"; /**< 参数的默认值 */
        std::string _errorInfo;    /**< 错误信息 */
        bool _existsInMap = false; /**< 参数是否存在于命令行参数映射中 */

      public:
        /**
         * @brief 默认构造函数被禁用
         */
        Rule() = delete;

        /**
         * @brief 双参数构造函数被禁用
         */
        Rule(const std::string &, const std::string &) = delete;
        
        /**
         * @brief 单参数构造函数被禁用
         */
        Rule(const std::string &) = delete;

        // Rule& operator=(const cppcli::Rule&) = delete;
        
        /**
         * @brief 构造函数，创建一个非必需的参数规则
         * 
         * @param shortParam 参数的短名称，如"-h"
         * @param longParam 参数的长名称，如"--help"
         * @param helpInfo 参数的帮助信息
         * 
         * @details 创建一个非必需的命令行参数规则，初始化参数的短名称、长名称和帮助信息
         */
        Rule(const std::string &shortParam, const std::string &longParam, const std::string helpInfo)
            : _shortParam(shortParam), _longParam(longParam), _helpInfo(helpInfo),
              _limitNumRange(std::make_pair(double(-1), double(-1))){};

        /**
         * @brief 构造函数，创建一个参数规则
         * 
         * @param shortParam 参数的短名称，如"-h"
         * @param longParam 参数的长名称，如"--help"
         * @param helpInfo 参数的帮助信息
         * @param necessary 参数是否必需
         * 
         * @details 创建一个命令行参数规则，初始化参数的短名称、长名称、帮助信息和是否必需
         */
        Rule(const std::string &shortParam, const std::string &longParam, const std::string helpInfo, bool necessary)
            : _shortParam(shortParam), _longParam(longParam), _helpInfo(helpInfo), _necessary(necessary),
              _limitNumRange(std::make_pair(double(-1), double(-1))){};

        /**
         * @brief 将参数值类型限制为整数
         * 
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 设置参数值类型为整数，在解析时会进行类型检查
         */
        Rule *limitInt();   /// valid after setting input type
        
        /**
         * @brief 将参数值类型限制为浮点数
         * 
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 设置参数值类型为浮点数，在解析时会进行类型检查
         */
        Rule *limitDouble();
        
        /**
         * @brief 将当前参数设置为帮助参数
         * 
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 将当前参数标记为帮助参数，当该参数出现时会显示帮助文档
         */
        Rule *asHelpParam();
        
        /**
         * @brief 检查参数是否存在于命令行中
         * 
         * @return bool 如果参数存在于命令行中返回true，否则返回false
         * 
         * @details 检查当前参数是否存在于用户输入的命令行参数中
         */
        bool exists();

        /**
         * @brief 获取参数值作为字符串
         * 
         * @tparam T 类型参数，必须为std::string
         * @return const std::string 参数值的字符串表示
         * 
         * @details 获取参数的输入值作为字符串返回
         */
        template <class T, class = typename std::enable_if<std::is_same<T, std::string>::value>::type>
        const std::string get()
        {
            return _inputValue;
        }

        /**
         * @brief 获取参数值作为整数
         * 
         * @tparam T 类型参数，必须为int
         * @return int 参数值的整数表示
         * 
         * @details 将参数的输入值转换为整数返回
         */
        template <class T, class = typename std::enable_if<std::is_same<T, int>::value>::type>
        int get()
        {
            return std::stoi(_inputValue);
        }

        /**
         * @brief 获取参数值作为浮点数
         * 
         * @tparam T 类型参数，必须为double
         * @return double 参数值的浮点数表示
         * 
         * @details 将参数的输入值转换为浮点数返回
         */
        template <class T, class = typename std::enable_if<std::is_same<T, double>::value>::type>
        double get()
        {
            return std::stod(_inputValue);
        }

        /**
         * @brief 限制参数值必须为指定选项之一
         * 
         * @tparam Args 可变参数类型
         * @param args 允许的选项列表
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 限制参数值必须是指定选项中的一个，如limitOneOf("yes", "no", "maybe")
         */
        template <class... Args>
        Rule *limitOneOf(Args... args)
        {
            std::ostringstream oss;

            auto addToVec = [this, &oss](auto i) {
                oss << i;
                _limitOneVec.push_back(std::move(oss.str()));
                oss.str("");
            };
            std::initializer_list<int>{(addToVec(args), 0)...};
            return this;
        }

        /**
         * @brief 限制数字参数的取值范围
         * 
         * @tparam T 数字类型，必须是int、float或double
         * @param min 最小值
         * @param max 最大值
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 限制数字参数的取值范围，如limitNumRange(1, 100)
         */
        template <class T,
                  class = typename std::enable_if<std::is_same<T, int>::value || std::is_same<T, float>::value ||
                                                  std::is_same<T, double>::value>::type>
        Rule *limitNumRange(T min, T max)
        {
            _limitNumRange = std::make_pair(double(min), double(max));
            return this;
        }

        /**
         * @brief 设置参数的默认值
         * 
         * @tparam T 默认值类型
         * @param defaultValue 默认值
         * @return Rule* 当前规则对象指针，用于链式调用
         * 
         * @details 设置参数的默认值，当参数没有在命令行中指定时使用该值
         */
        template <class T>
        Rule *setDefault(const T &defaultValue)
        {
            std::ostringstream oss;
            oss << defaultValue;
            _default = oss.str();
            return this;
        }

      private:
        /**
         * @brief 获取错误信息
         * 
         * @param errorEventType 错误事件类型
         * @return const std::string 格式化的错误信息
         * 
         * @details 根据错误事件类型生成相应的错误信息字符串
         */
        const std::string getError(cppcli::detail::ErrorEventType errorEventType);

        /**
         * @brief 构建帮助信息行
         * 
         * @return std::string 格式化的帮助信息行
         * 
         * @details 根据规则的各种属性构建一行格式化的帮助信息，用于显示在帮助文档中
         */
        std::string buildHelpInfoLine();

#ifdef CPPCLI_DEBUG
        /**
         * @brief 获取调试信息
         * 
         * @return std::string 包含规则详细信息的调试字符串
         * 
         * @details 返回规则的详细信息，包括参数名称、值、类型、限制条件等，用于调试
         */
        std::string debugInfo() const;
#endif
    };

}   // namespace cppcli

cppcli::HelpDocEnum cppcli::Rule::detail::HelpDocStruct::_helpDocType = cppcli::HelpDocEnum::USE_DEFAULT_HELPDOC;
cppcli::Rule *cppcli::Rule::detail::HelpDocStruct::rule = nullptr;

cppcli::Rule *cppcli::Rule::limitInt()
{
    _valueType = cppcli::detail::ValueTypeEnum::INT;
    return this;
}

cppcli::Rule *cppcli::Rule::limitDouble()
{
    _valueType = cppcli::detail::ValueTypeEnum::DOUBLE;
    return this;
}

cppcli::Rule *cppcli::Rule::asHelpParam()
{
    if (_necessary == true)
    {
        _necessary = false;
    }
    cppcli::Rule::detail::HelpDocStruct::rule = this;
    return this;
}

/**
 * @brief 检查规则对应的参数是否存在于命令行中
 * 
 * @return bool 如果参数存在返回true，否则返回false
 * 
 * @details 用于判断用户是否在命令行中提供了该参数
 */
bool cppcli::Rule::exists() { return _existsInMap; }

/**
 * @brief 获取特定错误类型的错误信息字符串
 * 
 * @param errorEventType 错误事件类型
 * @return const std::string 格式化的错误信息字符串
 * 
 * @details 根据不同的错误类型，生成相应的错误提示信息：
 * - 必需参数缺失错误：显示参数名称
 * - 值类型错误：显示期望的值类型
 * - 值不在预设集合中错误：显示所有有效的值选项
 * - 数值范围错误：显示有效的最小值和最大值
 */
const std::string cppcli::Rule::getError(cppcli::detail::ErrorEventType errorEventType)
{
    std::ostringstream oss;

    oss << "[";
    switch (errorEventType)
    {
    case cppcli::detail::ErrorEventType::MECESSARY_ERROR: {
        if (_longParam.empty())
            oss << _shortParam;
        else
            oss << _shortParam << std::move(" | ") << _longParam;
        break;
    }
    case cppcli::detail::ErrorEventType::VALUETYPE_ERROR: {
        if (_valueType == cppcli::detail::ValueTypeEnum::DOUBLE)
            oss << std::move(" NUMBER (DOUBLE) ");
        else if (_valueType == cppcli::detail::ValueTypeEnum::INT)
            oss << std::move(" NUMBER (INT) ");
        break;
    }
    case cppcli::detail::ErrorEventType::ONEOF_ERROR: {
        for (int i = 0; i < _limitOneVec.size(); i++)
        {
            if (i == (_limitOneVec.size() - 1))
            {
                oss << _limitOneVec.at(i);
                break;
            }
            oss << _limitOneVec.at(i) << std::move(" ");
        }

        break;
    }
    case cppcli::detail::ErrorEventType::NUMRANGE_ERROR: {
        oss << _limitNumRange.first << std::move("(MIN), ") << _limitNumRange.second << std::move("(MAX)");
        break;
    }
    }
    oss << "]";
    return std::move(oss.str());
}

/**
 * @brief 构建格式化的帮助信息行
 * 
 * @return std::string 格式化后的帮助信息行字符串
 * 
 * @details 根据当前规则的各项属性，构建一个格式化的帮助信息行，用于在命令行帮助文档中显示。
 * 信息行包括：
 * - 参数名称（短名称和长名称）
 * - 帮助描述信息
 * - 是否必需输入的标记
 * - 默认值信息
 * 
 * 对于较长的帮助文本，会自动进行换行处理，保证输出的美观性。第一行会包含完整信息，
 * 后续行只包含帮助文本的继续部分，并使用适当的缩进保持对齐。
 */
std::string cppcli::Rule::buildHelpInfoLine()
{
    std::ostringstream oss;

    int commandsDis = 28;
    int helpInfoDis = 36;
    int necessaryDis = 20;
    int defaultStrDis = 20;
    int theDis = 2;

    oss << std::setw(commandsDis) << std::left << (_longParam.empty() ? _shortParam : _shortParam + " | " + _longParam);

    int writeTime = _helpInfo.size() % helpInfoDis - theDis == 0 ? int(_helpInfo.size() / (helpInfoDis - theDis))
                                                                 : int((_helpInfo.size() / (helpInfoDis - theDis))) + 1;
    std::string necessaryOutStr = _necessary ? "true" : "false";
    std::string defaultValueOutStr = _default == "[EMPTY]" ? _default : "=" + _default;
 
    if (writeTime == 1)
    {
        oss << std::setw(helpInfoDis) << std::left << _helpInfo;
        oss << std::setw(necessaryDis) << std::left << "MUST-ENTER[" + necessaryOutStr + "]";
        oss << std::setw(defaultStrDis) << std::left << "DEFAULT->" + _default;
        oss << std::endl;
        return std::move(oss.str());
    }
    int pos = 0;
    for (int i = 0; i < writeTime; i++)
    {
        if (i == 0)
        {
            oss << std::setw(helpInfoDis) << std::setw(helpInfoDis) << _helpInfo.substr(pos, helpInfoDis - theDis);
            oss << std::setw(necessaryDis) << std::left << "MUST-ENTER[" + necessaryOutStr + "]";
            oss << std::setw(defaultStrDis) << std::left << "DEFAULT->" + _default;
            oss << std::endl;
            pos += helpInfoDis - theDis;
        }
        else
        {
            oss << std::setw(commandsDis + 4) << std::left << "";
            oss << _helpInfo.substr(pos, helpInfoDis - theDis);
            oss << std::endl;
            pos += helpInfoDis - theDis;
        }
    }

    return std::move(oss.str());
}

#ifdef CPPCLI_DEBUG
/**
 * @brief 获取规则对象的调试信息
 * 
 * @return std::string 包含规则详细信息的字符串
 * 
 * @details 生成一个包含规则对象所有重要属性的调试字符串，用于开发和调试目的。
 * 返回的信息包括：
 * - 参数名称（短名称和长名称）
 * - 输入值
 * - 是否必需
 * - 值类型
 * - 默认值
 * - 是否已存在于命令行中
 * - 可选值列表（如果有）
 * - 数字范围限制（如果有）
 * 
 * 该方法仅在定义了CPPCLI_DEBUG宏的情况下可用。
 */
std::string cppcli::Rule::debugInfo() const
{

    std::ostringstream oss;

    if (_longParam.empty())
    {
        oss << "command params --> " << _shortParam << std::endl;
    }
    else
    {
        oss << "command params --> " << _shortParam << "|" << _longParam << std::endl;
    }

    oss << "[CPPCLI_DEBUG]     input value = " << _inputValue << std::endl;
    oss << "[CPPCLI_DEBUG]     necessary = " << _necessary << std::endl;
    oss << "[CPPCLI_DEBUG]     valueType = " << _valueType << std::endl;
    oss << "[CPPCLI_DEBUG]     default = " << _default << std::endl;
    oss << "[CPPCLI_DEBUG]     exist = " << _existsInMap << std::endl;

    oss << "[CPPCLI_DEBUG]     limitOneVec = (";
    for (int i = 0; i < _limitOneVec.size(); i++)
    {
        if (i == _limitOneVec.size() - 1)
        {
            oss << _limitOneVec.at(i);
            break;
        }
        oss << _limitOneVec.at(i) << ", ";
    }
    oss << "), size=" << _limitOneVec.size() << std::endl;

    oss << "[CPPCLI_DEBUG]     limitNumRange = (" << _limitNumRange.first << " " << _limitNumRange.second << ")";

    return oss.str();
}
#endif

/* =================================================================================*/
/* =================================================================================*/
/* ===============================    Option    =====================================*/
/* =================================================================================*/
/* =================================================================================*/

namespace cppcli {

    /**
     * @class Option
     * @brief 命令行参数解析类
     * 
     * @details 该类是cppcli库的主要类，用于解析、验证和处理命令行参数。
     * 它允许用户定义参数规则，并自动处理参数解析、验证和错误处理。
     */
    class Option {
      private:
        /**
         * @class detail
         * @brief Option类的内部详细实现
         * 
         * @details 包含了Option类的各种验证方法的实现
         */
        class detail {
            detail() = delete; /**< 禁用默认构造函数 */
            detail(const detail &) = delete; /**< 禁用拷贝构造函数 */
            friend class cppcli::Option; /**< 允许Option类访问私有成员 */
            
            /**
             * @brief 验证必需参数
             * 
             * @param opt Option对象引用
             * @return int 错误参数的索引，如果没有错误返回-1
             * 
             * @details 检查所有必需参数是否存在于命令行中
             */
            static int necessaryVerify(Option &opt);
            
            /**
             * @brief 验证参数值类型
             * 
             * @param opt Option对象引用
             * @return int 错误参数的索引，如果没有错误返回-1
             * 
             * @details 检查所有参数的值是否符合指定的类型
             */
            static int valueTypeVerify(Option &opt);
            
            /**
             * @brief 验证数字参数的取值范围
             * 
             * @param opt Option对象引用
             * @return int 错误参数的索引，如果没有错误返回-1
             * 
             * @details 检查所有数字参数的值是否在指定的范围内
             */
            static int numRangeVerify(Option &opt);
            
            /**
             * @brief 验证参数值是否在指定选项中
             * 
             * @param opt Option对象引用
             * @return int 错误参数的索引，如果没有错误返回-1
             * 
             * @details 检查所有设置了可选值列表的参数，其值是否在指定的选项中
             */
            static int oneOfVerify(Option &opt);
        };

      public:
        /**
         * @brief 构造函数
         * 
         * @param argc 命令行参数数量
         * @param argv 命令行参数数组
         * 
         * @details 初始化Option对象，解析命令行参数并存储到内部映射中
         */
        Option(int argc, char *argv[]);
        
        /**
         * @brief 拷贝构造函数被禁用
         */
        Option(const cppcli::Option &) = delete;
        
        /**
         * @brief 赋值运算符被禁用
         */
        Option operator=(const cppcli::Option &) = delete;
        
        /**
         * @brief 添加一个非必需的参数规则
         * 
         * @param shortParam 参数的短名称，如"-h"
         * @param longParam 参数的长名称，如"--help"
         * @param helpInfo 参数的帮助信息
         * @return cppcli::Rule* 新创建的规则对象指针
         * 
         * @details 创建并添加一个非必需的参数规则，可以通过返回的指针进行链式调用设置其他属性
         */
        cppcli::Rule *operator()(const std::string &shortParam, const std::string &longParam,
                                 const std::string helpInfo);
        
        /**
         * @brief 添加一个参数规则
         * 
         * @param shortParam 参数的短名称，如"-h"
         * @param longParam 参数的长名称，如"--help"
         * @param helpInfo 参数的帮助信息
         * @param necessary 参数是否必需
         * @return cppcli::Rule* 新创建的规则对象指针
         * 
         * @details 创建并添加一个参数规则，可以指定参数是否必需，可以通过返回的指针进行链式调用设置其他属性
         */
        cppcli::Rule *operator()(const std::string &shortParam, const std::string &longParam,
                                 const std::string helpInfo, bool necessary);
        
        /**
         * @brief 析构函数
         * 
         * @details 释放所有分配的规则对象
         */
        ~Option();
        
        /**
         * @brief 解析命令行参数
         * 
         * @details 对命令行参数进行解析和验证，如果有错误则输出错误信息并退出
         */
        void parse();
        
        /**
         * @brief 检查参数是否存在于命令行中
         * 
         * @param shortParam 参数的短名称
         * @return bool 如果参数存在于命令行中返回true，否则返回false
         * 
         * @details 通过短名称检查参数是否存在于命令行中
         */
        bool exists(const std::string shortParam);
        
        /**
         * @brief 检查参数是否存在于命令行中
         * 
         * @param rule 参数规则对象指针
         * @return bool 如果参数存在于命令行中返回true，否则返回false
         * 
         * @details 通过规则对象指针检查参数是否存在于命令行中
         */
        bool exists(const cppcli::Rule *rule);





#ifdef CPPCLI_DEBUG
        /**
         * @brief 打印命令行参数映射
         * 
         * @details 在调试模式下打印命令行参数映射的内容
         */
        void printCommandMap();
#endif

        /**
         * @brief 获取工作路径
         * 
         * @return const std::string 当前程序的工作路径
         * 
         * @details 返回可执行文件所在的路径
         */
        const std::string getWorkPath();
        
        /**
         * @brief 获取执行路径
         * 
         * @return const std::string 当前程序的执行路径
         * 
         * @details 返回程序启动时的工作目录
         */
        const std::string getExecPath();

      private:
        /**
         * @brief 错误退出类型
         * 
         * @details 定义当解析参数发生错误时的退出行为，默认只打印规则信息然后退出
         */
        cppcli::ErrorExitEnum _exitType = cppcli::ErrorExitEnum::EXIT_PRINT_RULE;
        
        /**
         * @brief 命令行参数映射
         * 
         * @details 存储命令行参数的键值对，键为参数名（如"-h"或"--help"），值为参数值
         */
        std::map<std::string, std::string> _commandMap;
        
        /**
         * @brief 参数规则对象列表
         * 
         * @details 存储所有已定义的参数规则对象的指针，用于管理和验证命令行参数
         */
        std::vector<cppcli::Rule *> _ruleVec;

        /**
         * @brief 工作路径
         * 
         * @details 存储可执行文件所在的目录路径
         */
        std::string _workPath;
        
        /**
         * @brief 执行路径
         * 
         * @details 存储程序启动时的当前目录路径
         */
        std::string _execPath;

      private:
        /**
         * @brief 为所有规则获取输入值
         * 
         * @details 遍历所有规则，获取对应的输入值或使用默认值
         */
        void rulesGainInputValue();
        
        /**
         * @brief 获取规则对应的输入值
         * 
         * @param rule 要获取输入值的规则
         * @return std::string 规则对应的输入值
         * 
         * @details 根据规则的短参数名和长参数名在命令行参数映射中查找对应的值
         */
        std::string getInputValue(const cppcli::Rule &rule);
        
        /**
         * @brief 构建帮助文档
         * 
         * @return std::string 格式化的帮助文档字符串
         * 
         * @details 遍历所有规则，生成格式化的帮助信息
         */
        std::string buildHelpDoc();
        
        /**
         * @brief 打印帮助文档
         * 
         * @details 如果命令行中存在帮助参数，则打印帮助文档并退出程序
         */
        void printHelpDoc();
        
        /**
         * @brief 检查规则对应的参数是否存在于命令行中
         * 
         * @param rule 要检查的规则指针
         * @return bool 如果参数存在返回true，否则返回false
         * 
         * @details 检查给定规则的短参数名或长参数名是否在命令行参数映射中存在
         */
        bool mapExists(const cppcli::Rule *rule);
        
        /**
         * @brief 初始化路径信息
         * 
         * @param argc 命令行参数数量
         * @param argv 命令行参数数组
         * 
         * @details 初始化执行路径和工作路径，根据不同平台使用不同方法获取
         */
        void pathInit(int argc, char *argv[]);

        /**
         * @brief 错误退出函数
         * 
         * @param errorInfo 错误信息
         * @param index 错误参数的索引
         * @param exitType 退出类型
         * @param eventType 错误事件类型
         * 
         * @details 根据退出类型和错误事件类型执行不同的退出行为
         */
        void errorExitFunc(const std::string errorInfo, int index, cppcli::ErrorExitEnum exitType,
                           cppcli::detail::ErrorEventType eventType);

        /**
         * @brief 获取参数值（模板函数）
         * 
         * @tparam T 参数值类型
         * @param shortParam 参数的短名称
         * @return T 参数值
         * 
         * @details 根据参数的短名称获取对应的参数值
         */
        template <class T, class = typename std::enable_if<std::is_same<T, std::string>::value>::type>
        std::string get(const std::string shortParam)
        {
            for (cppcli::Rule *rule : _ruleVec)
            {
                if (rule->_shortParam == shortParam)
                {
                    return rule->get<std::string>();
                }
            }
            std::cout << "error: don't set where short-param = " << shortParam << std::endl;
            std::exit(-1);
        }

        template <class T, class = typename std::enable_if<std::is_same<T, int>::value>::type>
        int get(const std::string shortParam)
        {
            for (cppcli::Rule *rule : _ruleVec)
            {
                if (rule->_shortParam == shortParam)
                {
                    return rule->get<int>();
                }
            }
            std::cout << "error: don't set where short-param = " << shortParam << std::endl;
            std::exit(-1);
        }

        template <class T, class = typename std::enable_if<std::is_same<T, double>::value>::type>
        double get(const std::string shortParam)
        {
            for (cppcli::Rule *rule : _ruleVec)
            {
                if (rule->_shortParam == shortParam)
                {
                    return rule->get<double>();
                }
            }
            std::cout << "error: don't set where short-param = " << shortParam << std::endl;
            std::exit(-1);
        }

        template <class T, class = typename std::enable_if<std::is_same<T, std::string>::value>::type>
        std::string get(cppcli::Rule *rule)
        {
            return rule->get<std::string>();
        }

        template <class T, class = typename std::enable_if<std::is_same<T, int>::value>::type>
        int get(cppcli::Rule *rule)
        {
            return rule->get<int>();
        }

        template <class T, class = typename std::enable_if<std::is_same<T, double>::value>::type>
        double get(cppcli::Rule *rule)
        {
            return rule->get<double>();
        }

    };

}   // namespace cppcli

/**
 * @brief 验证必需参数是否存在
 * 
 * @param opt Option对象引用
 * @return int 如果验证失败，返回失败规则的索引；如果验证通过，返回-1
 * 
 * @details 验证所有标记为必需的参数是否存在于命令行中。
 * 验证流程：
 * 1. 遍历所有参数规则
 * 2. 检查每个必需参数是否存在于命令行中
 * 3. 如果有必需参数缺失，返回其索引
 * 4. 如果所有必需参数都存在，返回-1表示验证通过
 */
int cppcli::Option::Option::detail::necessaryVerify(Option &opt)
{
    cppcli::Rule *rule = nullptr;
    for (int index = 0; index < opt._ruleVec.size(); index ++)
    {
        rule = opt._ruleVec.at(index);
        if (rule->_necessary && !opt.mapExists(rule))
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in necessaryVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif
            return index;
        }
    }
    return -1;
};

/**
 * @brief 验证参数值类型是否正确
 * 
 * @param opt Option对象引用
 * @return int 如果验证失败，返回失败规则的索引；如果验证通过，返回-1
 * 
 * @details 验证所有限制了值类型的参数是否满足类型要求。
 * 验证流程：
 * 1. 遍历所有参数规则
 * 2. 如果参数类型是字符串或参数不存在，直接跳过
 * 3. 如果参数类型是整数，验证其值是否是有效整数
 * 4. 如果参数类型是浮点数，验证其值是否是有效数字（整数或浮点数）
 * 5. 如果有参数值类型不正确，返回其索引
 * 6. 如果所有参数值类型都正确，返回-1表示验证通过
 */
int cppcli::Option::Option::detail::valueTypeVerify(Option &opt)
{
    cppcli::Rule *rule = nullptr;
    for (int index = 0; index < opt._ruleVec.size(); index ++)
    {
        rule = opt._ruleVec.at(index);
        if (rule->_valueType == cppcli::detail::ValueTypeEnum::STRING || !opt.mapExists(rule))
        {
            continue;
        }

        if (rule->_valueType == cppcli::detail::ValueTypeEnum::INT &&
            !cppcli::detail::algoUtil::isInt(rule->_inputValue))
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in valueTypeVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif
           
            return index;
        }

        if (rule->_valueType == cppcli::detail::ValueTypeEnum::DOUBLE &&
            !cppcli::detail::algoUtil::verifyDouble(rule->_inputValue))
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in valueTypeVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif
            return index;
        }

    }
    return -1;
}

/**
 * @brief 验证数值参数是否在指定范围内
 * 
 * @param opt Option对象引用
 * @return int 如果验证失败，返回失败规则的索引；如果验证通过，返回-1
 * 
 * @details 验证设置了数值范围限制的参数是否在指定范围内。
 * 验证流程：
 * 1. 遍历所有参数规则
 * 2. 如果参数类型是字符串或参数不存在，直接跳过
 * 3. 如果参数没有设置数值范围限制（min和max都是-1），直接跳过
 * 4. 验证参数值是否是有效的数字
 * 5. 验证参数值是否在指定的最小值和最大值之间
 * 6. 如果有参数值超出范围，返回其索引
 * 7. 如果所有参数值都在范围内，返回-1表示验证通过
 */
int cppcli::Option::Option::detail::numRangeVerify(Option &opt)
{
    cppcli::Rule *rule = nullptr;
    for (int index = 0; index < opt._ruleVec.size(); index ++)
    {
        rule = opt._ruleVec.at(index);
        if (rule->_valueType == cppcli::detail::ValueTypeEnum::STRING || !opt.mapExists(rule))
        {
            continue;
        }

        // no set it
        if (rule->_limitNumRange.first == -1 && rule->_limitNumRange.second == -1)
        {
            continue;
        }

        if(rule->_inputValue.empty() || !cppcli::detail::algoUtil::verifyDouble(rule->_inputValue))
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in numRangeVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif
            return index;
        }

        if (std::stod(rule->_inputValue) < rule->_limitNumRange.first ||
            std::stod(rule->_inputValue) > rule->_limitNumRange.second)
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in numRangeVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif
            return index;
        }
    }
    return -1;
}

/**
 * @brief 验证参数是否在给定的值列表中
 * 
 * @param opt Option对象
 * @return int 如果验证失败，返回失败的规则索引；否则返回-1
 * 
 * @details 遍历所有规则，检查每个规则的_limitOneVec是否为空。
 * 如果规则的_limitOneVec不为空且参数存在于命令行中，则检查参数值是否在_limitOneVec中。
 * 如果参数值不在_limitOneVec中，则返回失败的规则索引。
 */
int cppcli::Option::Option::detail::oneOfVerify(Option &opt)
{

    cppcli::Rule *rule = nullptr;
    for (int index = 0; index < opt._ruleVec.size(); index ++)
    {
        rule = opt._ruleVec.at(index);
        if (rule->_limitOneVec.size() == 0 || !opt.mapExists(rule))
        {
            continue;
        }

        if (std::find(rule->_limitOneVec.begin(), rule->_limitOneVec.end(), rule->_inputValue) ==
            rule->_limitOneVec.end())
        {

#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("failed in oneOfVerify, fail rule in following");
            CPPCLI_DEBUG_PRINT(rule->debugInfo(), "\n");
#endif

            return index;
        }
    }
    return -1;
}

#ifdef CPPCLI_DEBUG
/**
 * @brief 打印命令行参数映射表
 * 
 * @details 打印命令行参数映射表，包括映射表的大小和每个映射项的键值对。
 */
void cppcli::Option::Option::printCommandMap()
{
    CPPCLI_DEBUG_PRINT("-- commandMap, size = ", _commandMap.size());
    for (const std::pair<std::string, std::string> &pr : _commandMap)
    {
        CPPCLI_DEBUG_PRINT("    ", pr.first, "=", pr.second);
    }
    CPPCLI_DEBUG_PRINT("-- end commandMap");
}
#endif

/**
 * @brief 错误处理函数
 * 
 * @param errorInfo 错误信息
 * @param index 失败规则的索引
 * @param exitType 错误退出类型
 * @param eventType 错误事件类型
 * 
 * @details 根据错误类型和事件类型，输出错误信息并根据需要退出程序。
 * 如果是帮助文档请求，输出帮助文档并退出；否则输出错误信息并根据错误类型退出。
 */
void cppcli::Option::errorExitFunc(const std::string errorInfo, int index, cppcli::ErrorExitEnum exitType,
                                   cppcli::detail::ErrorEventType eventType)
{

    cppcli::Rule rule = *_ruleVec.at(index);

    // std::unique_lock<std::mutex> lock(cppcli::_coutMutex);
    std::ostringstream oss;
    if (eventType != cppcli::detail::ErrorEventType::MECESSARY_ERROR)
        oss << ", where command param = [" << rule._shortParam << "]";
    if (cppcli::Rule::detail::HelpDocStruct::rule != nullptr)
    {
        oss << std::endl << "Use [" << cppcli::Rule::detail::HelpDocStruct::rule->_shortParam << "] gain help doc";
    }

    std::cout << errorInfo << rule.getError(eventType) << oss.str() << std::endl;
    if (exitType == cppcli::EXIT_PRINT_RULE_HELPDOC)
        std::cout << buildHelpDoc();

    std::exit(0);
}

/**
 * @brief 构造函数
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * 
 * @details 初始化工作路径和执行路径，
 */
cppcli::Option::Option(int argc, char *argv[])
{
    // init work path and exec path
    pathInit(argc, argv);

#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- argc argv start");
    auto beStr = [&]() -> const std::string {
        std::ostringstream oss;
        for (int i = 0; i < argc; i++)
            oss << argv[i] << "  ";
        return oss.str();
    };
    CPPCLI_DEBUG_PRINT("argc = ", argc, " || argv = ", beStr());
#endif

    _ruleVec.reserve(64);

    // command params save to map
    cppcli::detail::algoUtil::InitCommandMap(argc, argv, _commandMap);
#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- argv map start");
    printCommandMap();
#endif
}

/**
 * @brief 析构函数
 * 
 * @details 清理所有规则对象并释放内存。
 */
cppcli::Option::~Option()
{
    for (cppcli::Rule *rule : _ruleVec)
    {
        if (rule != nullptr)
        {
            delete (rule);
        }
    }
    _ruleVec.clear();
}

/**
 * @brief 操作符重载，用于创建规则对象
 * 
 * @param shortParam 短参数
 * @param longParam 长参数
 * @param helpInfo 帮助信息
 * 
 * @return cppcli::Rule* 创建的规则对象
 * 
 * @details 检查短参数是否包含"-"，长参数是否为空或包含"-"。
 * 如果短参数不包含"-"，输出错误信息并退出。
 * 如果长参数不为空且不包含"-"，输出错误信息并退出。
 */
cppcli::Rule *cppcli::Option::operator()(const std::string &shortParam, const std::string &longParam,
                                         const std::string helpInfo)
{
    if (shortParam.find("-") == shortParam.npos)
    {
        std::cout << "short-param must contains \"-\" " << std::endl;
        std::exit(-1);
    }
    if (!longParam.empty() && longParam.find("-") == longParam.npos)
    {
        std::cout << "long-param must empty or contains \"-\" " << std::endl;
        std::exit(-1);
    }


    _ruleVec.push_back(new cppcli::Rule(shortParam, longParam, helpInfo));
    return _ruleVec.back();
}

/**
 * @brief 操作符重载，用于创建规则对象
 * 
 * @param shortParam 短参数
 * @param longParam 长参数
 * @param helpInfo 帮助信息
 * @param necessary 是否为必需参数
 * 
 * @return cppcli::Rule* 创建的规则对象
 * 
 * @details 检查短参数是否包含"-"，长参数是否为空或包含"-"。
 * 如果短参数不包含"-"，输出错误信息并退出。
 * 如果长参数不为空且不包含"-"，输出错误信息并退出。
 */
cppcli::Rule *cppcli::Option::operator()(const std::string &shortParam, const std::string &longParam,
                                         const std::string helpInfo, bool necessary)
{
    if (shortParam.find("-") == shortParam.npos)
    {
        std::cout << "short-param must contains \"-\" " << std::endl;
        std::exit(-1);
    }
    if (!longParam.empty() && longParam.find("-") == longParam.npos)
    {
        std::cout << "long-param must empty or contains \"-\" " << std::endl;
        std::exit(-1);
    }

    _ruleVec.push_back(new cppcli::Rule(shortParam, longParam, helpInfo, necessary));
    return _ruleVec.back();
}

/**
 * @brief 初始化工作路径和执行路径
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * 
 * @details 获取当前工作目录和可执行文件的路径，并存储在成员变量中。
 * 
 * @note 在Windows系统上使用_getcwd和GetModuleFileName函数。
 * 在Unix系统上使用getcwd和readlink函数。
 */
void cppcli::Option::pathInit(int argc, char *argv[])
{

    char execBuf[1024];
    char workBuf[1024];
#if defined(WIN32) || defined(_WIN64) || defined(__WIN32__)
    _getcwd(execBuf, sizeof(execBuf));
    GetModuleFileName(NULL, workBuf, sizeof(workBuf));
#else
    auto none1 = getcwd(execBuf, sizeof(execBuf));
    auto none2 = readlink("/proc/self/exe", workBuf, sizeof(workBuf));
#endif
    _execPath = execBuf;
    _workPath = std::move(cppcli::detail::pathUtil::getFileDir(workBuf));

#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("execPath = ", _execPath, ", workPath = ", _workPath);
#endif
}

/**
 * @brief 获取参数的输入值
 * 
 * @param rule 参数规则
 * 
 * @return std::string 参数的输入值
 * 
 * @details 根据规则的短参数和长参数在命令行映射表中的值，返回对应的输入值。
 * 如果短参数存在，返回短参数对应的值；否则返回长参数对应的值。
 */
std::string cppcli::Option::getInputValue(const cppcli::Rule &rule)
{

    std::string inputValue;
    if (_commandMap.find(rule._shortParam) != _commandMap.end())
    {
        inputValue = _commandMap[rule._shortParam];
    }
    if (_commandMap.find(rule._longParam) != _commandMap.end())
    {
        inputValue = _commandMap[rule._longParam];
    }

    return inputValue;
}

/**
 * @brief 获取所有规则的输入值
 * 
 * @details 遍历所有规则，检查命令行映射表中是否存在对应的短参数或长参数。
 * 如果存在，则设置规则的_existsInMap为true，并获取对应的输入值。
 * 如果输入值不为空，则设置规则的_inputValue为输入值。
 * 如果输入值为空且规则有默认值，则设置规则的_inputValue为默认值。
 */
void cppcli::Option::rulesGainInputValue()
{
    std::string inputValue;

    for (cppcli::Rule *rule : _ruleVec)
    {
        if (!mapExists(rule))
            continue;
            
        rule->_existsInMap = true;
        inputValue = getInputValue(*rule);
        
        if (!inputValue.empty())
        {
            rule->_inputValue = inputValue;
            
            continue;
        }
        if(inputValue.empty() && rule->_default != "[EMPTY]")
        {
            rule->_inputValue = rule->_default;
        }
    }
}

/**
 * @brief 检查规则是否存在于命令行映射表中
 * 
 * @param rule 参数规则
 * 
 * @return true 如果规则存在于命令行映射表中
 * @return false 如果规则不存在于命令行映射表中
 * 
 * @details 检查规则的短参数和长参数是否存在于命令行映射表中。
 * 如果存在其中任何一个，则返回true；否则返回false。
 */
bool cppcli::Option::mapExists(const cppcli::Rule *rule)
{
    if (rule != nullptr)
    {
        return _commandMap.find(rule->_shortParam) != _commandMap.end() ||
               _commandMap.find(rule->_longParam) != _commandMap.end();
    }
    return false;
}

/**
 * @brief 检查规则是否存在于命令行映射表中
 * 
 * @param rule 参数规则
 * 
 * @return true 如果规则存在于命令行映射表中
 * @return false 如果规则不存在于命令行映射表中
 * 
 * @details 检查规则的短参数和长参数是否存在于命令行映射表中。
 * 如果存在其中任何一个，则返回true；否则返回false。
 */
bool cppcli::Option::exists(const cppcli::Rule *rule)
{
#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- exist rule");
    CPPCLI_DEBUG_PRINT(rule->debugInfo());
#endif
    return mapExists(rule);
}

/**
 * @brief 检查短参数是否存在于命令行映射表中
 * 
 * @param shortParam 短参数
 * 
 * @return true 如果短参数存在于命令行映射表中
 * @return false 如果短参数不存在于命令行映射表中
 * 
 * @details 遍历所有规则，检查短参数是否存在于命令行映射表中。
 * 如果存在，则返回true；否则返回false。
 */
bool cppcli::Option::exists(const std::string shortParam)
{

    for (cppcli::Rule *rule : _ruleVec)
    {
        if (rule->_shortParam == shortParam)
        {
#ifdef CPPCLI_DEBUG
            CPPCLI_DEBUG_PRINT("---------------- exist rule");
            CPPCLI_DEBUG_PRINT(rule->debugInfo());
#endif
            return mapExists(rule);
        }
    }
    return false;
}

/**
 * @brief 构建帮助文档
 * 
 * @return std::string 构建的帮助文档字符串
 * 
 * @details 遍历所有规则，构建帮助文档字符串。
 * 每个规则的帮助文档信息将被添加到字符串中。
 */
std::string cppcli::Option::buildHelpDoc()
{
    std::ostringstream oss;
    oss << "options:" << std::endl;
    for (cppcli::Rule *rule : _ruleVec)
    {
        oss << rule->buildHelpInfoLine();
    }
    return oss.str();
}

/**
 * @brief 打印帮助文档
 * 
 * @details 如果帮助文档规则存在，则打印帮助文档并退出程序。
 * 如果帮助文档规则不存在，则不执行任何操作。
 */
void cppcli::Option::printHelpDoc()
{
#ifdef CPPCLI_DEBUG
    if (nullptr == cppcli::Rule::detail::HelpDocStruct::rule)
    {
        CPPCLI_DEBUG_PRINT("warning: you don't set help param\n");
    }
#endif

    if (!mapExists(cppcli::Rule::detail::HelpDocStruct::rule))
    {
        return;
    }

    std::cout << buildHelpDoc();
    std::exit(0);
}

/**
 * @brief 获取工作路径
 * 
 * @return const std::string 工作路径
 */
const std::string cppcli::Option::getWorkPath() { return _workPath; }

/**
 * @brief 获取执行路径
 * 
 * @return const std::string 执行路径
 */
const std::string cppcli::Option::getExecPath() { return _execPath; }

/**
 * @brief 解析命令行参数
 * 
 * @details 遍历所有规则，检查命令行映射表中是否存在对应的短参数或长参数。
 * 如果存在，则设置规则的_existsInMap为true，并获取对应的输入值。
 * 如果输入值不为空，则设置规则的_inputValue为输入值。
 * 如果输入值为空且规则有默认值，则设置规则的_inputValue为默认值。
 */
void cppcli::Option::parse()
{
    // rules save Unique correspondence input value
    rulesGainInputValue();

#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- rules vector start");
    for (int i = 0; i < _ruleVec.size(); i++)
    {
        CPPCLI_DEBUG_PRINT("vec index = ", i, "  ", _ruleVec[i]->debugInfo());
    }

#endif

    // check param exists of help , if exists print it
    printHelpDoc();

    int necessaryResult = Option::detail::necessaryVerify(*this);
    int valueTypeResult = Option::detail::valueTypeVerify(*this);
    int oneOfResult = Option::detail::oneOfVerify(*this);
    int numRangeResult = Option::detail::numRangeVerify(*this);

#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- verify result");
    CPPCLI_DEBUG_PRINT("necessaryResult=", necessaryResult, ", valueTypeResult=", valueTypeResult,
                       ",oneOfResult=", oneOfResult, ", numRangeResult", numRangeResult);

#endif

    if (necessaryResult > -1)
    {
        errorExitFunc("Must enter this param: ", necessaryResult, _exitType,
                      cppcli::detail::ErrorEventType::MECESSARY_ERROR);
    }

    if (valueTypeResult > -1)
    {
        errorExitFunc("Please enter the correct type: ", valueTypeResult, _exitType,
                      cppcli::detail::ErrorEventType::VALUETYPE_ERROR);
    }

    if (oneOfResult > -1)
    {
        errorExitFunc("Must be one of these values: ", oneOfResult, _exitType,
                      cppcli::detail::ErrorEventType::ONEOF_ERROR);
    }

    if (numRangeResult > -1)
    {
        errorExitFunc("Must be within this range: ", numRangeResult, _exitType,
                      cppcli::detail::ErrorEventType::NUMRANGE_ERROR);
    }

#ifdef CPPCLI_DEBUG
    CPPCLI_DEBUG_PRINT("---------------- parse result");
    CPPCLI_DEBUG_PRINT(">>>>>>>>>   PASS   <<<<<<<<<<");
#endif
}
