#pragma once
#include <fstream>  
#include <boost/property_tree/ptree.hpp>  
#include <boost/property_tree/ini_parser.hpp>  
#include <boost/filesystem.hpp>    
#include <map>
#include <iostream>

/**
 * @brief INI配置文件中单个Section的数据存储结构
 * 
 * 对应INI文件中的一个节（如 [Redis]、[MySql]），
 * 内部以 key-value 形式存储该节下的所有配置项。
 */
struct SectionInfo {
	/// 默认构造函数
	SectionInfo(){}
	/// 析构函数，清空配置数据
	~SectionInfo(){
		_section_datas.clear();
	}
	
	/// 拷贝构造函数
	SectionInfo(const SectionInfo& src) {
		_section_datas = src._section_datas;
	}
	
	/// 拷贝赋值运算符
	SectionInfo& operator = (const SectionInfo& src) {
		if (&src == this) {
			return *this;
		}

		this->_section_datas = src._section_datas;
		return *this;
	}

	/// 存储当前节下所有配置项的映射（key -> value）
	std::map<std::string, std::string> _section_datas;

	/**
	 * @brief 下标运算符，根据配置项名称获取对应的值
	 * @param key 配置项名称
	 * @return 配置项的值，若不存在则返回空字符串
	 */
	std::string  operator[](const std::string  &key) {
		if (_section_datas.find(key) == _section_datas.end()) {
			return "";
		}
		// 这里可以添加一些边界检查  
		return _section_datas[key];
	}

	/**
	 * @brief 根据配置项名称获取对应的值
	 * @param key 配置项名称
	 * @return 配置项的值，若不存在则返回空字符串
	 */
	std::string GetValue(const std::string & key) {
		if (_section_datas.find(key) == _section_datas.end()) {
			return "";
		}
		// 这里可以添加一些边界检查  
		return _section_datas[key];
	}
};

/**
 * @brief 配置文件管理器（单例）
 * 
 * 负责加载和解析 INI 格式的配置文件（config.ini），
 * 提供按 section 和 key 查询配置值的接口。
 * 使用 Boost.PropertyTree 解析INI文件，程序启动时自动加载。
 */
class ConfigMgr
{
public:
	/// 析构函数，清空配置映射表
	~ConfigMgr() {
		_config_map.clear();
	}

	/**
	 * @brief 下标运算符，根据节名获取对应的SectionInfo
	 * @param section INI文件中的节名称（如 "Redis"、"MySql"）
	 * @return 对应的SectionInfo对象，若节不存在则返回空的SectionInfo
	 */
	SectionInfo operator[](const std::string& section) {
		if (_config_map.find(section) == _config_map.end()) {
			return SectionInfo();
		}
		return _config_map[section];
	}

	/// 拷贝赋值运算符
	ConfigMgr& operator=(const ConfigMgr& src) {
		if (&src == this) {
			return *this;
		}

		this->_config_map = src._config_map;
	};

	/// 拷贝构造函数
	ConfigMgr(const ConfigMgr& src) {
		this->_config_map = src._config_map;
	}

	/**
	 * @brief 获取ConfigMgr单例实例（Meyers' Singleton，线程安全）
	 * @return ConfigMgr单例引用
	 */
	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

	/**
	 * @brief 根据节名和配置项名获取配置值
	 * @param section 节名称
	 * @param key 配置项名称
	 * @return 配置值字符串
	 */
	std::string GetValue(const std::string& section, const std::string & key);

private:
	/// 私有构造函数，在初始化时加载并解析INI配置文件
	ConfigMgr();
	/// 配置数据存储映射，键为节名(section)，值为该节下的所有key-value对
	std::map<std::string, SectionInfo> _config_map;
};

