#include "ConfigMgr.h"
#include "ResourceUploadFile.h" //llfc::EnsureDirectoryDurable（P2 CAS 根目录耐久创建）
#include <cstdlib>              //std::exit（目录不可用即致命失败）

ConfigMgr::ConfigMgr(){
	// 获取当前工作目录  
	boost::filesystem::path current_path = boost::filesystem::current_path();
	// 构建config.ini文件的完整路径  
	boost::filesystem::path config_path = current_path / "config.ini";
	std::cout << "Config path: " << config_path << std::endl;

	// 使用Boost.PropertyTree来读取INI文件  
	boost::property_tree::ptree pt;
	boost::property_tree::read_ini(config_path.string(), pt);


	// 遍历INI文件中的所有section  
	for (const auto& section_pair : pt) {
		const std::string& section_name = section_pair.first;
		const boost::property_tree::ptree& section_tree = section_pair.second;

		// 对于每个section，遍历其所有的key-value对  
		std::map<std::string, std::string> section_config;
		for (const auto& key_value_pair : section_tree) {
			const std::string& key = key_value_pair.first;
			const std::string& value = key_value_pair.second.get_value<std::string>();
			section_config[key] = value;
		}
		SectionInfo sectionInfo;
		sectionInfo._section_datas = section_config;
		// 将section的key-value对保存到config_map中  
		_config_map[section_name] = sectionInfo;
	}

	// 输出所有的section和key-value对  
	for (const auto& section_entry : _config_map) {
		const std::string& section_name = section_entry.first;
		SectionInfo section_config = section_entry.second;
		std::cout << "[" << section_name << "]" << std::endl;
		for (const auto& key_value_pair : section_config._section_datas) {
			std::cout << key_value_pair.first << "=" << key_value_pair.second << std::endl;
		}
	}


	InitPath();
}

std::string ConfigMgr::GetValue(const std::string& section, const std::string& key) {
	if (_config_map.find(section) == _config_map.end()) {
		return "";
	}

	return _config_map[section].GetValue(key);
}

boost::filesystem::path ConfigMgr::GetFileOutPath()
{
	return _static_path;
}

boost::filesystem::path ConfigMgr::GetResourceRootPath()
{
	return _resource_root;
}

void ConfigMgr::InitPath()
{
	// 获取当前工作目录
	boost::filesystem::path current_path = boost::filesystem::current_path();
	std::string bindir = _config_map["Output"].GetValue("Path");
	std::string staticdir = _config_map["Static"].GetValue("Path");
	_static_path = current_path / bindir / staticdir;
	_bin_path = current_path / bindir;
	//资源消息根目录与头像目录隔离：清理任务只扫 resource/，不会误伤头像。
	//可配置：优先 [Storage] ResourceRoot，其次 [Output] ResourceRoot，都缺省
	//时回落历史行为 <Output.Path>/resource。绝对路径原样采用；相对路径与
	//[Output] Path 同一基准（进程工作目录）解析。
	std::string resource_root_cfg = (*this)["Storage"].GetValue("ResourceRoot");
	if (resource_root_cfg.empty()) {
		resource_root_cfg = (*this)["Output"].GetValue("ResourceRoot");
	}
	if (!resource_root_cfg.empty()) {
		const boost::filesystem::path configured(resource_root_cfg);
		_resource_root = configured.is_absolute()
			? configured : current_path / configured;
	}
	else {
		_resource_root = current_path / bindir / "resource";
	}


	// 检查路径是否存在
	if (!boost::filesystem::exists(_static_path)) {
		// 如果路径不存在，创建它
		if (boost::filesystem::create_directories(_static_path)) {
			std::cout << "路径已成功创建: " << _static_path.string() << std::endl;
		}
		else {
			std::cerr << "创建路径失败: " << _static_path.string() << std::endl;
		}
	}
	else {
		std::cout << "路径已存在: " << _static_path.string() << std::endl;
	}

	//P2 内容寻址存储根 + 子目录：**必须耐久创建**（目录条目落盘），失败即
	//致命——根目录不可用意味着上传/下载/清理全部不可用，绝不允许"报了错
	//还继续跑"。EnsureDirectoryDurable 在 POSIX 下逐级 fsync 整条祖先链
	//（既覆盖本次新建，也覆盖"上次建目录成功但父目录 fsync 失败"的重试），
	//Windows 只负责建树（目录条目靠 MoveFileExW 写穿语义）。
	//说明：这里对 CAS 根与两个子目录各调一次，重复 fsync 同一批目录是可接受
	//的启动期成本；两处调用都不接受"失败但继续"。
	if (!llfc::EnsureDirectoryDurable(_resource_root)) {
		std::cerr << "资源目录不可用（持久化创建失败）: " << _resource_root.string()
			<< "（CAS 根目录不可用则本进程无法提供资源服务，直接失败退出）"
			<< std::endl;
		std::exit(EXIT_FAILURE);
	}
	std::cout << "资源目录: " << _resource_root.string() << std::endl;
	for (const char* sub : {"uploading", "blobs"}) {
		if (!llfc::EnsureDirectoryDurable(_resource_root / sub)) {
			std::cerr << "创建资源子目录失败: " << (_resource_root / sub).string()
				<< "（上传/发布路径不可用，直接失败退出）" << std::endl;
			std::exit(EXIT_FAILURE);
		}
	}
}
