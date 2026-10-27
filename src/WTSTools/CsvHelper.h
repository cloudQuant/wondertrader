/*!
 * \file CsvHelper.h
 * \brief CSV文件读取辅助工具
 * 
 * 该文件定义了CSV文件读取相关的类和方法，主要用于解析和访问CSV格式的数据文件
 */

#pragma once
#include <string.h>
#include <string>
#include <unordered_map>
#include <stdint.h>
#include <fstream>
#include <vector>
#include <sstream>

/*!
 * \brief CSV文件读取器类
 * 
 * 该类提供了CSV文件的读取、解析和数据访问功能，支持按列名或列索引获取不同类型的数据
 */
class CsvReader
{
public:
    /*!
     * \brief 构造函数
     * \param item_splitter 列分隔符，默认为逗号
     */
    CsvReader(const char* item_splitter = ",");

public:
    /*!
     * \brief 从文件加载CSV数据
     * \param filename 文件路径
     * \return 加载是否成功
     * 
     * 该方法会读取CSV文件的第一行作为字段名，并构建字段名到列索引的映射
     */
    bool	load_from_file(const char* filename);

public:
    /*!
     * \brief 获取列数
     * \return 当前CSV文件的列数
     */
    inline uint32_t	col_count() { return (uint32_t)_fields_map.size(); }

    /*!
     * \brief 获取指定列的32位有符号整数值
     * \param col 列索引
     * \return 整数值，如果列不存在则返回0
     */
    int32_t		get_int32(int32_t col);
    
    /*!
     * \brief 获取指定列的32位无符号整数值
     * \param col 列索引
     * \return 无符号整数值，如果列不存在则返回0
     */
    uint32_t	get_uint32(int32_t col);

    /*!
     * \brief 获取指定列的64位有符号整数值
     * \param col 列索引
     * \return 整数值，如果列不存在则返回0
     */
    int64_t		get_int64(int32_t col);
    
    /*!
     * \brief 获取指定列的64位无符号整数值
     * \param col 列索引
     * \return 无符号整数值，如果列不存在则返回0
     */
    uint64_t	get_uint64(int32_t col);

    /*!
     * \brief 获取指定列的双精度浮点数值
     * \param col 列索引
     * \return 浮点数值，如果列不存在则返回0
     */
    double		get_double(int32_t col);

    /*!
     * \brief 获取指定列的字符串值
     * \param col 列索引
     * \return 字符串值，如果列不存在则返回空字符串
     */
    const char*	get_string(int32_t col);

    /*!
     * \brief 根据字段名获取32位有符号整数值
     * \param field 字段名
     * \return 整数值，如果字段不存在则返回0
     */
    int32_t		get_int32(const char* field);
    
    /*!
     * \brief 根据字段名获取32位无符号整数值
     * \param field 字段名
     * \return 无符号整数值，如果字段不存在则返回0
     */
    uint32_t	get_uint32(const char* field);

    /*!
     * \brief 根据字段名获取64位有符号整数值
     * \param field 字段名
     * \return 整数值，如果字段不存在则返回0
     */
    int64_t		get_int64(const char* field);
    
    /*!
     * \brief 根据字段名获取64位无符号整数值
     * \param field 字段名
     * \return 无符号整数值，如果字段不存在则返回0
     */
    uint64_t	get_uint64(const char* field);

    /*!
     * \brief 根据字段名获取双精度浮点数值
     * \param field 字段名
     * \return 浮点数值，如果字段不存在则返回0
     */
    double		get_double(const char* field);

    /*!
     * \brief 根据字段名获取字符串值
     * \param field 字段名
     * \return 字符串值，如果字段不存在则返回空字符串
     */
    const char*	get_string(const char* field);

    /*!
     * \brief 读取下一行数据
     * \return 是否成功读取到下一行数据
     * 
     * 该方法读取CSV文件的下一行数据，并将其解析为单元格数组
     */
    bool		next_row();

    /*!
     * \brief 获取所有字段名
     * \return 逗号分隔的字段名字符串
     * 
     * 该方法返回CSV文件的所有字段名，以逗号分隔
     */
    const char* fields() const 
    { 
        static std::string s;
        if(s.empty())
        {
            std::stringstream ss;
            for (auto item : _fields_map)
                ss << item.first << ",";

            s = ss.str();
            s = s.substr(0, s.size() - 1);
        }

        return s.c_str();
    }

private:
    /*!
     * \brief 检查列索引是否有效
     * \param col 列索引
     * \return 列索引是否有效
     */
    bool		check_cell(int32_t col);
    
    /*!
     * \brief 根据字段名获取列索引
     * \param field 字段名
     * \return 列索引，如果字段不存在则返回INT_MAX
     */
    int32_t		get_col_by_filed(const char* field);

private:
    std::ifstream	_ifs;            ///< 文件输入流
    char			_buffer[1024];       ///< 行缓冲区
    std::string		_item_splitter;  ///< 列分隔符

    std::unordered_map<std::string, int32_t> _fields_map;  ///< 字段名到列索引的映射
    std::vector<std::string> _current_cells;               ///< 当前行的单元格数据
};
