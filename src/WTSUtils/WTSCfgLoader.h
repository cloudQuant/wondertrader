/*!
 * \file WTSCfgLoader.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 配置加载器头文件
 *
 * 本文件定义了WTSCfgLoader类，用于加载和解析配置文件。
 * 支持JSON和YAML格式的配置文件解析，并将其转换为WTSVariant对象供程序使用。
 */
#pragma once
#include "../Includes/WTSMarcos.h"

#include <string>

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 配置加载器类
 * 
 * 提供了从文件或内容字符串加载配置的方法
 * 支持JSON和YAML格式的配置文件解析，自动识别文件扩展名
 * 并将解析后的配置转换为WTSVariant对象供程序使用
 */
class WTSCfgLoader
{
	/**
	 * @brief 从字符串解析JSON格式的配置
	 * 
	 * 将JSON格式的字符串解析为WTSVariant对象
	 * 
	 * @param content JSON格式的字符串
	 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
	 */
	static WTSVariant*	load_from_json(const char* content);

	/**
	 * @brief 从字符串解析YAML格式的配置
	 * 
	 * 将YAML格式的字符串解析为WTSVariant对象
	 * 
	 * @param content YAML格式的字符串
	 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
	 */
	static WTSVariant*	load_from_yaml(const char* content);

public:
	/**
	 * @brief 从文件加载配置
	 * 
	 * 根据文件扩展名自动选择JSON或YAML解析器解析配置文件
	 * 支持.json、.yaml和.yml扩展名的文件
	 * 自动处理文件编码问题，支持UTF-8和GBK编码
	 * 
	 * @param filename 配置文件路径
	 * @return WTSVariant* 解析后的可变类型对象，文件不存在或解析失败时返回NULL
	 */
	static WTSVariant*	load_from_file(const char* filename);

	/**
	 * @brief 从字符串内容加载配置
	 * 
	 * 根据指定的格式解析字符串内容
	 * 自动处理字符串编码问题，支持UTF-8和GBK编码
	 * 
	 * @param content 配置内容字符串
	 * @param isYaml 是否为YAML格式，默认为false（JSON格式）
	 * @return WTSVariant* 解析后的可变类型对象，解析失败时返回NULL
	 */
	static WTSVariant*	load_from_content(const std::string& content, bool isYaml = false);

	/**
	 * @brief 从文件加载配置（std::string版本）
	 * 
	 * 此方法为load_from_file的重载版本，接受std::string类型的文件名
	 * 
	 * @param filename 配置文件路径（std::string类型）
	 * @return WTSVariant* 解析后的可变类型对象，文件不存在或解析失败时返回NULL
	 */
	static WTSVariant*	load_from_file(const std::string& filename)
	{
		return load_from_file(filename.c_str());
	}
};

