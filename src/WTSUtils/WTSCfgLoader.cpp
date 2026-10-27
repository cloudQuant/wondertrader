/*!
 * \file WTSCfgLoader.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 配置加载器实现文件
 *
 * 本文件实现了WTSCfgLoader类，提供了JSON和YAML格式的配置文件解析功能。
 * 包含了将JSON和YAML数据转换为WTSVariant对象的转换函数和编码处理功能。
 */
#include "WTSCfgLoader.h"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../Share/charconv.hpp"

#include "../Includes/WTSVariant.hpp"
#include <rapidjson/document.h>
namespace rj = rapidjson;


/**
 * @brief 将rapidjson解析的JSON数据转换为WTSVariant对象
 * 
 * 递归遍历JSON数据结构，将其转换为WTSVariant对象
 * 支持对象、数组、字符串、数字和布尔类型的转换
 * 
 * @param root rapidjson解析的JSON数据节点
 * @param params 输出的WTSVariant对象，用于存储转换后的数据
 * @return bool 转换是否成功
 */
bool json_to_variant(const rj::Value& root, WTSVariant* params)
{
	if (root.IsObject() && params->type() != WTSVariant::VT_Object)
		return false;

	if (root.IsArray() && params->type() != WTSVariant::VT_Array)
		return false;

	if (root.IsObject())
	{
		for (auto& m : root.GetObject())
		{
			const char* key = m.name.GetString();
			const rj::Value& item = m.value;
			switch (item.GetType())
			{
			case rj::kObjectType:
			{
				WTSVariant* subObj = WTSVariant::createObject();
				if (json_to_variant(item, subObj))
					params->append(key, subObj, false);
			}
			break;
			case rj::kArrayType:
			{
				WTSVariant* subAy = WTSVariant::createArray();
				if (json_to_variant(item, subAy))
					params->append(key, subAy, false);
			}
			break;
			case rj::kNumberType:
				if (item.IsInt())
					params->append(key, item.GetInt());
				else if (item.IsUint())
					params->append(key, item.GetUint());
				else if (item.IsInt64())
					params->append(key, item.GetInt64());
				else if (item.IsUint64())
					params->append(key, item.GetUint64());
				else if (item.IsDouble())
					params->append(key, item.GetDouble());
				break;
			case rj::kStringType:
				params->append(key, item.GetString());
				break;
			case rj::kTrueType:
			case rj::kFalseType:
				params->append(key, item.GetBool());
				break;

			}
		}
	}
	else
	{
		for (auto& item : root.GetArray())
		{
			switch (item.GetType())
			{
			case rj::kObjectType:
			{
				WTSVariant* subObj = WTSVariant::createObject();
				if (json_to_variant(item, subObj))
					params->append(subObj, false);
			}
			break;
			case rj::kArrayType:
			{
				WTSVariant* subAy = WTSVariant::createArray();
				if (json_to_variant(item, subAy))
					params->append(subAy, false);
			}
			break;
			case rj::kNumberType:
				if (item.IsInt())
					params->append(item.GetInt());
				else if (item.IsUint())
					params->append(item.GetUint());
				else if (item.IsInt64())
					params->append(item.GetInt64());
				else if (item.IsUint64())
					params->append(item.GetUint64());
				else if (item.IsDouble())
					params->append(item.GetDouble());
				break;
			case rj::kStringType:
				params->append(item.GetString());
				break;
			case rj::kTrueType:
			case rj::kFalseType:
				params->append(item.GetBool());
				break;
			}
		}
	}
	return true;
}

/**
 * @brief 从字符串解析JSON格式的配置
 * 
 * 使用rapidjson库解析JSON字符串，并将结果转换为WTSVariant对象
 * 
 * @param content JSON格式的字符串
 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
 */
WTSVariant* WTSCfgLoader::load_from_json(const char* content)
{
	rj::Document root;
	root.Parse(content);

	if (root.HasParseError())
		return NULL;

	WTSVariant* ret = WTSVariant::createObject();
	if (!json_to_variant(root, ret))
	{
		ret->release();
		return NULL;
	}

	return ret;
}

#include "../WTSUtils/yamlcpp/yaml.h"
/**
 * @brief 将YAML::Node转换为WTSVariant对象
 * 
 * 递归遍历YAML数据结构，将其转换为WTSVariant对象
 * 支持映射、序列和标量类型的转换
 * 
 * @param root YAML::Node类型的YAML数据节点
 * @param params 输出的WTSVariant对象，用于存储转换后的数据
 * @return bool 转换是否成功
 */
bool yaml_to_variant(const YAML::Node& root, WTSVariant* params)
{
	if (root.IsNull() && params->type() != WTSVariant::VT_Object)
		return false;

	if (root.IsSequence() && params->type() != WTSVariant::VT_Array)
		return false;

	bool isMap = root.IsMap();
	for (auto& m : root)
	{
		std::string key = isMap ? m.first.as<std::string>() : "";
		const YAML::Node& item = isMap ? m.second : m;
		switch (item.Type())
		{
		case YAML::NodeType::Map:
		{
			WTSVariant* subObj = WTSVariant::createObject();
			if (yaml_to_variant(item, subObj))
			{
				if(isMap)
					params->append(key.c_str(), subObj, false);
				else
					params->append(subObj, false);
			}
		}
		break;
		case YAML::NodeType::Sequence:
		{
			WTSVariant* subAy = WTSVariant::createArray();
			if (yaml_to_variant(item, subAy))
			{
				if (isMap)
					params->append(key.c_str(), subAy, false);
				else
					params->append(subAy, false);
			}
		}
		break;
		case YAML::NodeType::Scalar:
			if (isMap)
				params->append(key.c_str(), item.as<std::string>().c_str());
			else
				params->append(item.as<std::string>().c_str());
			break;
		}
	}

	return true;
}

/**
 * @brief 从字符串解析YAML格式的配置
 * 
 * 使用yaml-cpp库解析YAML字符串，并将结果转换为WTSVariant对象
 * 
 * @param content YAML格式的字符串
 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
 */
WTSVariant* WTSCfgLoader::load_from_yaml(const char* content)
{
	YAML::Node root = YAML::Load(content);

	if (root.IsNull())
		return NULL;

	WTSVariant* ret = WTSVariant::createObject();
	if (!yaml_to_variant(root, ret))
	{
		ret->release();
		return NULL;
	}

	return ret;
}

/**
 * @brief 从字符串内容加载配置
 * 
 * 根据指定的格式解析字符串内容
 * 自动处理字符串编码问题，支持UTF-8和GBK编码
 * 在Windows平台上会将UTF-8转换为GBK，在Linux平台上会将GBK转换为UTF-8
 * 
 * @param content 配置内容字符串
 * @param isYaml 是否为YAML格式，默认为false（JSON格式）
 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
 */
WTSVariant* WTSCfgLoader::load_from_content(const std::string& content, bool isYaml /* = false */)
{
	//加一个自动检测编码的逻辑
	bool isUTF8 = EncodingHelper::isUtf8((unsigned char*)content.data(), content.size());

	std::string buffer;
	//Linux下得是UTF8
	//Win下得是GBK
#ifdef _WIN32
	if (isUTF8)
		buffer = UTF8toChar(content);
#else
	if (!isUTF8)
		buffer = ChartoUTF8(content);
#endif

	if (buffer.empty())
		buffer = content;

	if (isYaml)
		return load_from_yaml(buffer.c_str());
	else
		return load_from_json(buffer.c_str());
}

/**
 * @brief 从文件加载配置
 * 
 * 根据文件扩展名自动选择JSON或YAML解析器解析配置文件
 * 支持.json、.yaml和.yml扩展名的文件
 * 自动处理文件编码问题，在Windows上将UTF-8转换为GBK，在Linux上将GBK转换为UTF-8
 * 
 * @param filename 配置文件路径
 * @return WTSVariant* 解析后的可变类型对象，文件不存在或解析失败时返回NULL
 */
WTSVariant* WTSCfgLoader::load_from_file(const char* filename)
{
	if (!StdFile::exists(filename))
		return NULL;

	std::string content;
	StdFile::read_file_content(filename, content);
	if (content.empty())
		return NULL;

	//加一个自动检测编码的逻辑
	bool isUTF8 = EncodingHelper::isUtf8((unsigned char*)content.data(), content.size());

	//By Wesley @ 2022.01.07
	//Linux下得是UTF8
	//Win下得是GBK
#ifdef _WIN32
	if(isUTF8)
		content = UTF8toChar(content);
#else
	if (!isUTF8)
		content = ChartoUTF8(content);
#endif

	if (StrUtil::endsWith(filename, ".json"))
		return load_from_json(content.c_str());
	else if (StrUtil::endsWith(filename, ".yaml") || StrUtil::endsWith(filename, ".yml"))
		return load_from_yaml(content.c_str());

	return NULL;
}
