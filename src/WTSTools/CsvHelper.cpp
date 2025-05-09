/*!
 * \file CsvHelper.cpp
 * \brief CSV文件读取辅助工具实现
 * 
 * 该文件实现了CSV文件读取相关的功能，包括文件解析、数据访问等
 */

#include "CsvHelper.h"

#include <limits.h>

#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"

/*!
 * \brief CsvReader类构造函数
 * \param item_splitter 列分隔符，默认为逗号
 * 
 * 初始化CSV读取器，设置列分隔符
 */
CsvReader::CsvReader(const char* item_splitter /* = "," */)
	: _item_splitter(item_splitter)
{

}

/*!
 * \brief 从文件加载CSV数据
 * \param filename 文件路径
 * \return 加载是否成功
 * 
 * 该方法首先检查文件是否存在，然后打开文件并读取第一行作为字段名。
 * 处理过程包括：
 * 1. 检测并跳过UTF-8 BOM标记
 * 2. 清除特殊字符
 * 3. 将字段名转为小写
 * 4. 分割并存储字段名到列索引的映射
 */
bool CsvReader::load_from_file(const char* filename)
{
	if (!StdFile::exists(filename))
		return false;

	_ifs.open(filename);

	_ifs.getline(_buffer, 1024);
	//判断是不是UTF-8BOM 编码
	static char flag[] = { (char)0xEF, (char)0xBB, (char)0xBF };
	char* buf = _buffer;
	if (memcmp(_buffer, flag, sizeof(char) * 3) == 0)
		buf += 3;

	std::string row = buf;

	//替换掉一些字段的特殊符号
	StrUtil::replace(row, "<", "");
	StrUtil::replace(row, ">", "");
	StrUtil::replace(row, "\"", "");
	StrUtil::replace(row, "'", "");

	//将字段名转成小写
	StrUtil::toLowerCase(row);

	StringVector fields = StrUtil::split(row, _item_splitter.c_str());
	for (uint32_t i = 0; i < fields.size(); i++)
	{
		StrUtil::trim(fields[i], " ");
		StrUtil::trim(fields[i], "\n");
		StrUtil::trim(fields[i], "\t");
		StrUtil::trim(fields[i], "\r");
		if (fields[i].empty())
			break;

		_fields_map[fields[i]] = i;
	}

	return true;
}

/*!
 * \brief 读取下一行数据
 * \return 是否成功读取到下一行数据
 * 
 * 该方法读取CSV文件的下一行数据，跳过空行，并将非空行解析为单元格数组
 */
bool CsvReader::next_row()
{
	if (_ifs.eof())
		return false;

	while (!_ifs.eof())
	{
		_ifs.getline(_buffer, 1024);
		if(strlen(_buffer) == 0)
			continue;
		else
			break;
	} 
	
	if (strlen(_buffer) == 0)
		return false;
	_current_cells.clear();
	StrUtil::split(_buffer, _current_cells, _item_splitter.c_str());
	return true;
}

/*!
 * \brief 获取指定列的整型值
 * \param col 列索引
 * \return 整型值
 * 
 * 该方法检查指定列是否存在，然后将列值转换为整型返回
 */
int32_t CsvReader::get_int32(int32_t col)
{
	if (!check_cell(col))
		return 0;

	return strtol(_current_cells[col].c_str(), NULL, 10);
}

/*!
 * \brief 获取指定列的32位无符号整数值
 * \param col 列索引
 * \return 无符号整数值，如果列不存在则返回0
 */
uint32_t CsvReader::get_uint32(int32_t col)
{
	if (!check_cell(col))
		return 0;

	return strtoul(_current_cells[col].c_str(), NULL, 10);
}

/*!
 * \brief 获取指定列的64位有符号整数值
 * \param col 列索引
 * \return 整数值，如果列不存在则返回0
 */
int64_t CsvReader::get_int64(int32_t col)
{
	if (!check_cell(col))
		return 0;

	return strtoll(_current_cells[col].c_str(), NULL, 10);
}

/*!
 * \brief 获取指定列的64位无符号整数值
 * \param col 列索引
 * \return 无符号整数值，如果列不存在则返回0
 */
uint64_t CsvReader::get_uint64(int32_t col)
{
	if (!check_cell(col))
		return 0;

	return strtoull(_current_cells[col].c_str(), NULL, 10);
}

/*!
 * \brief 获取指定列的双精度浮点数值
 * \param col 列索引
 * \return 浮点数值，如果列不存在则返回0
 */
double CsvReader::get_double(int32_t col)
{
	if (!check_cell(col))
		return 0;

	return strtod(_current_cells[col].c_str(), NULL);
}

/*!
 * \brief 获取指定列的字符串值
 * \param col 列索引
 * \return 字符串值，如果列不存在则返回空字符串
 */
const char* CsvReader::get_string(int32_t col)
{
	if (!check_cell(col))
		return "";

	return _current_cells[col].c_str();
}

/*!
 * \brief 根据字段名获取32位有符号整数值
 * \param field 字段名
 * \return 整数值，如果字段不存在则返回0
 */
int32_t CsvReader::get_int32(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_int32(col);
}

/*!
 * \brief 根据字段名获取32位无符号整数值
 * \param field 字段名
 * \return 无符号整数值，如果字段不存在则返回0
 */
uint32_t CsvReader::get_uint32(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_uint32(col);
}

/*!
 * \brief 根据字段名获取64位有符号整数值
 * \param field 字段名
 * \return 整数值，如果字段不存在则返回0
 */
int64_t CsvReader::get_int64(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_int64(col);
}

/*!
 * \brief 根据字段名获取64位无符号整数值
 * \param field 字段名
 * \return 无符号整数值，如果字段不存在则返回0
 */
uint64_t CsvReader::get_uint64(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_uint64(col);
}

/*!
 * \brief 根据字段名获取双精度浮点数值
 * \param field 字段名
 * \return 浮点数值，如果字段不存在则返回0
 */
double CsvReader::get_double(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_double(col);
}

/*!
 * \brief 根据字段名获取字符串值
 * \param field 字段名
 * \return 字符串值，如果字段不存在则返回空字符串
 */
const char* CsvReader::get_string(const char* field)
{
	int32_t col = get_col_by_filed(field);
	return get_string(col);
}

/*!
 * \brief 检查列索引是否有效
 * \param col 列索引
 * \return 列索引是否有效
 * 
 * 该方法检查列索引是否在有效范围内，包括检查是否为INT_MAX（表示字段不存在）
 * 以及是否在0到字段数量之间
 */
bool CsvReader::check_cell(int32_t col)
{
	if (col == INT_MAX )
		return false;

	if (col < 0 || col >= (int32_t)_fields_map.size())
		return false;

	return true;
}

/*!
 * \brief 根据字段名获取列索引
 * \param field 字段名
 * \return 列索引，如果字段不存在则返回INT_MAX
 * 
 * 该方法在字段映射表中查找指定字段名，返回对应的列索引
 * 如果字段不存在，则返回INT_MAX作为特殊标记
 */
int32_t CsvReader::get_col_by_filed(const char* field)
{
	auto it = _fields_map.find(field);
	if (it == _fields_map.end())
		return INT_MAX;

	return it->second;
}