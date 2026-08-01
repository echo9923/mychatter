#include "RedisMgr.h"
#include "const.h"
#include "ConfigMgr.h"
#include "DistLock.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;
RedisMgr::RedisMgr() {
	auto& gCfgMgr = ConfigMgr::Inst();
	auto host = gCfgMgr["Redis"]["Host"];
	auto port = gCfgMgr["Redis"]["Port"];
	auto pwd = gCfgMgr["Redis"]["Passwd"];
	_con_pool.reset(new RedisConPool(10, host, atoi(port.c_str()), pwd));
}

RedisMgr::~RedisMgr() {
	
}



bool RedisMgr::Get(const std::string& key, std::string& value)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	 auto reply = (redisReply*)redisCommand(connect, "GET %s", key.c_str());
	 if (reply == NULL) {
		 std::cout << "[ GET  " << key << " ] failed" << std::endl;
		// freeReplyObject(reply);
		 _con_pool->returnConnection(connect);
		  return false;
	}

	 if (reply->type != REDIS_REPLY_STRING) {
		 std::cout << "[ GET  " << key << " ] failed" << std::endl;
		 freeReplyObject(reply);
		 _con_pool->returnConnection(connect);
		 return false;
	}

	 value = reply->str;
	 freeReplyObject(reply);

	 std::cout << "Succeed to execute command [ GET " << key << "  ]" << std::endl;
	 _con_pool->returnConnection(connect);
	 return true;
}

bool RedisMgr::Set(const std::string &key, const std::string &value){
	//执行redis命令行
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "SET %s %s", key.c_str(), value.c_str());

	//如果返回NULL则说明执行失败
	if (NULL == reply)
	{
		std::cout << "Execut command [ SET " << key << "  "<< value << " ] failure ! " << std::endl;
		//freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	//如果执行失败则释放连接
	if (!(reply->type == REDIS_REPLY_STATUS && (strcmp(reply->str, "OK") == 0 || strcmp(reply->str, "ok") == 0)))
	{
		std::cout << "Execut command [ SET " << key << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	//执行成功 释放redisCommand执行后返回的redisReply所占用的内存
	freeReplyObject(reply);
	std::cout << "Execut command [ SET " << key << "  " << value << " ] success ! " << std::endl;
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::SetExp(const std::string& key, const std::string& value, int expire_seconds) {
	//执行redis命令行
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "SETEX %s %d %s", key.c_str(), 
		      expire_seconds,
		value.c_str());

	if (NULL == reply) {
		std::cout << "Execute command [ SETEX " << key << " " << expire_seconds
			<< " " << value << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (!(reply->type == REDIS_REPLY_STATUS &&
		(strcmp(reply->str, "OK") == 0 || strcmp(reply->str, "ok") == 0))) {
		std::cout << "Execute command [ SETEX " << key << " " << expire_seconds
			<< " " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	freeReplyObject(reply);
	std::cout << "Execute command [ SETEX " << key << " " << expire_seconds
		<< " " << value << " ] success ! " << std::endl;
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::LPush(const std::string &key, const std::string &value)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "LPUSH %s %s", key.c_str(), value.c_str());
	if (NULL == reply)
	{
		std::cout << "Execut command [ LPUSH " << key << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0) {
		std::cout << "Execut command [ LPUSH " << key << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	std::cout << "Execut command [ LPUSH " << key << "  " << value << " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::LPop(const std::string &key, std::string& value){
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "LPOP %s ", key.c_str());
	if (reply == nullptr ) {
		std::cout << "Execut command [ LPOP " << key<<  " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type == REDIS_REPLY_NIL) {
		std::cout << "Execut command [ LPOP " << key << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	value = reply->str;
	std::cout << "Execut command [ LPOP " << key <<  " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::RPush(const std::string& key, const std::string& value) {
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "RPUSH %s %s", key.c_str(), value.c_str());
	if (NULL == reply)
	{
		std::cout << "Execut command [ RPUSH " << key << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0) {
		std::cout << "Execut command [ RPUSH " << key << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	std::cout << "Execut command [ RPUSH " << key << "  " << value << " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}
bool RedisMgr::RPop(const std::string& key, std::string& value) {
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "RPOP %s ", key.c_str());
	if (reply == nullptr ) {
		std::cout << "Execut command [ RPOP " << key << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type == REDIS_REPLY_NIL) {
		std::cout << "Execut command [ RPOP " << key << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}
	value = reply->str;
	std::cout << "Execut command [ RPOP " << key << " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::HSet(const std::string &key, const std::string &hkey, const std::string &value) {
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "HSET %s %s %s", key.c_str(), hkey.c_str(), value.c_str());
	if (reply == nullptr ) {
		std::cout << "Execut command [ HSet " << key << "  " << hkey <<"  " << value << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER) {
		std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << value << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << value << " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	 const char* argv[4];
	 size_t argvlen[4];
	 argv[0] = "HSET";
	argvlen[0] = 4;
	argv[1] = key;
	argvlen[1] = strlen(key);
	argv[2] = hkey;
	argvlen[2] = strlen(hkey);
	argv[3] = hvalue;
	argvlen[3] = hvaluelen;

	auto reply = (redisReply*)redisCommandArgv(connect, 4, argv, argvlen);
	if (reply == nullptr ) {
		std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER) {
		std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}
	std::cout << "Execut command [ HSet " << key << "  " << hkey << "  " << hvalue << " ] success ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

std::string RedisMgr::HGet(const std::string &key, const std::string &hkey)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return "";
	}
	const char* argv[3];
	size_t argvlen[3];
	argv[0] = "HGET";
	argvlen[0] = 4;
	argv[1] = key.c_str();
	argvlen[1] = key.length();
	argv[2] = hkey.c_str();
	argvlen[2] = hkey.length();
	
	auto reply = (redisReply*)redisCommandArgv(connect, 3, argv, argvlen);
	if (reply == nullptr ) {
		std::cout << "Execut command [ HGet " << key << " "<< hkey <<"  ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return "";
	}

	if ( reply->type == REDIS_REPLY_NIL) {
		freeReplyObject(reply);
		std::cout << "Execut command [ HGet " << key << " " << hkey << "  ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return "";
	}

	std::string value = reply->str;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	std::cout << "Execut command [ HGet " << key << " " << hkey << " ] success ! " << std::endl;
	return value;
}

bool RedisMgr::HDel(const std::string& key, const std::string& field)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}

	Defer defer([&connect, this]() {
		_con_pool->returnConnection(connect);
		});

	redisReply* reply = (redisReply*)redisCommand(connect, "HDEL %s %s", key.c_str(), field.c_str());
	if (reply == nullptr) {
		std::cerr << "HDEL command failed" << std::endl;
		return false;
	}

	bool success = false;
	if (reply->type == REDIS_REPLY_INTEGER) {
		success = reply->integer > 0;
	}

	freeReplyObject(reply);
	return success;
}

bool RedisMgr::Del(const std::string &key)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	auto reply = (redisReply*)redisCommand(connect, "DEL %s", key.c_str());
	if (reply == nullptr ) {
		std::cout << "Execut command [ Del " << key <<  " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if ( reply->type != REDIS_REPLY_INTEGER) {
		std::cout << "Execut command [ Del " << key << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	std::cout << "Execut command [ Del " << key << " ] success ! " << std::endl;
	 freeReplyObject(reply);
	 _con_pool->returnConnection(connect);
	 return true;
}

bool RedisMgr::ExistsKey(const std::string &key)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}

	auto reply = (redisReply*)redisCommand(connect, "exists %s", key.c_str());
	if (reply == nullptr ) {
		std::cout << "Not Found [ Key " << key << " ]  ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_INTEGER || reply->integer == 0) {
		std::cout << "Not Found [ Key " << key << " ]  ! " << std::endl;
		_con_pool->returnConnection(connect);
		freeReplyObject(reply);
		return false;
	}
	std::cout << " Found [ Key " << key << " ] exists ! " << std::endl;
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::ZAdd(const std::string& key, long long score, const std::string& member)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	// score 与 member 均以独立 argv 元素传递，避免 %s 风格被空格/特殊字符拆分
	std::string score_str = std::to_string(score);
	const char* argv[4];
	size_t argvlen[4];
	argv[0] = "ZADD";
	argvlen[0] = 4;
	argv[1] = key.c_str();
	argvlen[1] = key.length();
	argv[2] = score_str.c_str();
	argvlen[2] = score_str.length();
	argv[3] = member.c_str();
	argvlen[3] = member.length();

	auto reply = (redisReply*)redisCommandArgv(connect, 4, argv, argvlen);
	if (reply == nullptr) {
		std::cout << "Execut command [ ZAdd " << key << " " << score << " " << member << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	bool success = (reply->type != REDIS_REPLY_ERROR);
	if (!success) {
		std::cout << "Execut command [ ZAdd " << key << " " << score << " " << member << " ] failure ! " << std::endl;
	}
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return success;
}

bool RedisMgr::ZRangeByScore(const std::string& key, long long exclusive_min, int limit, std::vector<std::string>& members)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	// 排他下界以 "(" 前缀表示 > min，+inf 与 LIMIT 0 limit 一并作为独立 argv 元素
	std::string min_str = "(" + std::to_string(exclusive_min);
	std::string offset_str = "0";
	std::string limit_str = std::to_string(limit);
	const char* argv[7];
	size_t argvlen[7];
	argv[0] = "ZRANGEBYSCORE";
	argvlen[0] = 13;
	argv[1] = key.c_str();
	argvlen[1] = key.length();
	argv[2] = min_str.c_str();
	argvlen[2] = min_str.length();
	argv[3] = "+inf";
	argvlen[3] = 4;
	argv[4] = "LIMIT";
	argvlen[4] = 5;
	argv[5] = offset_str.c_str();
	argvlen[5] = offset_str.length();
	argv[6] = limit_str.c_str();
	argvlen[6] = limit_str.length();

	auto reply = (redisReply*)redisCommandArgv(connect, 7, argv, argvlen);
	if (reply == nullptr) {
		std::cout << "Execut command [ ZRangeByScore " << key << " " << exclusive_min << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	if (reply->type != REDIS_REPLY_ARRAY) {
		std::cout << "Execut command [ ZRangeByScore " << key << " " << exclusive_min << " ] failure ! " << std::endl;
		freeReplyObject(reply);
		_con_pool->returnConnection(connect);
		return false;
	}

	// 逐元素以 string 填入 members，空数组也视为成功
	members.clear();
	for (size_t i = 0; i < reply->elements; ++i) {
		redisReply* el = reply->element[i];
		if (el != nullptr && el->str != nullptr) {
			members.emplace_back(el->str, el->len);
		}
	}
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return true;
}

bool RedisMgr::ZRem(const std::string& key, const std::string& member)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	const char* argv[3];
	size_t argvlen[3];
	argv[0] = "ZREM";
	argvlen[0] = 4;
	argv[1] = key.c_str();
	argvlen[1] = key.length();
	argv[2] = member.c_str();
	argvlen[2] = member.length();

	auto reply = (redisReply*)redisCommandArgv(connect, 3, argv, argvlen);
	if (reply == nullptr) {
		std::cout << "Execut command [ ZRem " << key << " " << member << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	bool success = (reply->type != REDIS_REPLY_ERROR);
	if (!success) {
		std::cout << "Execut command [ ZRem " << key << " " << member << " ] failure ! " << std::endl;
	}
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return success;
}

bool RedisMgr::Expire(const std::string& key, int seconds)
{
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}
	std::string sec_str = std::to_string(seconds);
	const char* argv[3];
	size_t argvlen[3];
	argv[0] = "EXPIRE";
	argvlen[0] = 6;
	argv[1] = key.c_str();
	argvlen[1] = key.length();
	argv[2] = sec_str.c_str();
	argvlen[2] = sec_str.length();

	auto reply = (redisReply*)redisCommandArgv(connect, 3, argv, argvlen);
	if (reply == nullptr) {
		std::cout << "Execut command [ Expire " << key << " " << seconds << " ] failure ! " << std::endl;
		_con_pool->returnConnection(connect);
		return false;
	}

	bool success = (reply->type != REDIS_REPLY_ERROR);
	if (!success) {
		std::cout << "Execut command [ Expire " << key << " " << seconds << " ] failure ! " << std::endl;
	}
	freeReplyObject(reply);
	_con_pool->returnConnection(connect);
	return success;
}


std::string RedisMgr::acquireLock(const std::string& lockName,
	int lockTimeout, int acquireTimeout) {

	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return "";
	}

	Defer defer([&connect, this]() {
		_con_pool->returnConnection(connect);
	});

	return DistLock::Inst().acquireLock(connect, lockName, lockTimeout, acquireTimeout);
}

bool RedisMgr::releaseLock(const std::string& lockName,
	const std::string& identifier) {
	if (identifier.empty()) {
		return true;
	}
	auto connect = _con_pool->getConnection();
	if (connect == nullptr) {
		return false;
	}


	Defer defer([&connect, this]() {
		_con_pool->returnConnection(connect);
		});

	return DistLock::Inst().releaseLock(connect, lockName, identifier);
}

bool RedisMgr::SetFileInfo(const std::string& name, std::shared_ptr<FileInfo> file_info)
{
	json root;
	root["file_path_str"] = file_info->_file_path_str;
	root["name"] = file_info->_name;
	root["seq"] = file_info->_seq;
	root["total_size"] = std::to_string(file_info->_total_size);
	root["trans_size"] = std::to_string(file_info->_trans_size);
	auto file_info_str = root.dump(4);
	auto redis_key = "file_upload_" + name;
	bool success = SetExp(redis_key, file_info_str, 3600);
	return success;
}

bool RedisMgr::SetDownLoadInfo(const std::string& name, std::shared_ptr<FileInfo> file_info) {
	json root;
	root["file_path_str"] = file_info->_file_path_str;
	root["name"] = file_info->_name;
	root["seq"] = file_info->_seq;
	root["total_size"] = std::to_string(file_info->_total_size);
	root["trans_size"] = std::to_string(file_info->_trans_size);
	auto file_info_str = root.dump(4);
	auto redis_key = "file_download_" + name;
	bool success = SetExp(redis_key, file_info_str, 3600);
	return success;
}

bool RedisMgr::DelDownLoadInfo(const std::string& name) {
	auto redis_key = "file_download_" + name;
	return Del(redis_key);
}

std::shared_ptr<FileInfo> RedisMgr::GetFileInfo(const std::string& name) {
	auto redis_key = "file_upload_" + name;
	std::string file_info_str = "";

	// 从 Redis 获取数据
	bool success = Get(redis_key, file_info_str);
	if (!success || file_info_str.empty()) {
		return nullptr;
	}

	// 解析 JSON
	auto root = json::parse(file_info_str, nullptr, false);
	if (root.is_discarded()) {
		std::cout << "Failed to parse file info JSON for name: " << name << std::endl;
		return nullptr;
	}

	// 创建 FileInfo 对象并填充数据
	auto file_info = std::make_shared<FileInfo>();
	try {
		file_info->_file_path_str = root["file_path_str"].get<std::string>();
		file_info->_name = root["name"].get<std::string>();
		file_info->_seq = root["seq"].get<int>();
		file_info->_total_size = std::stoll(root["total_size"].get<std::string>());
		file_info->_trans_size = std::stoll(root["trans_size"].get<std::string>());
	}
	catch (const std::exception& e) {
		std::cout << "Error parsing file info fields for name " << name << ": " << e.what() << std::endl;
		return nullptr;
	}

	return file_info;
}


std::shared_ptr<FileInfo> RedisMgr::GetDownloadInfo(const std::string& name) {
	auto redis_key = "file_download_" + name;
	std::string file_info_str = "";

	// 从 Redis 获取数据
	bool success = Get(redis_key, file_info_str);
	if (!success || file_info_str.empty()) {
		return nullptr;
	}

	// 解析 JSON
	auto root = json::parse(file_info_str, nullptr, false);
	if (root.is_discarded()) {
		std::cout << "Failed to parse file info JSON for name: " << name << std::endl;
		return nullptr;
	}

	// 创建 FileInfo 对象并填充数据
	auto file_info = std::make_shared<FileInfo>();
	try {
		file_info->_file_path_str = root["file_path_str"].get<std::string>();
		file_info->_name = root["name"].get<std::string>();
		file_info->_seq = root["seq"].get<int>();
		file_info->_total_size = std::stoll(root["total_size"].get<std::string>());
		file_info->_trans_size = std::stoll(root["trans_size"].get<std::string>());
	}
	catch (const std::exception& e) {
		std::cout << "Error parsing file info fields for name " << name << ": " << e.what() << std::endl;
		return nullptr;
	}

	return file_info;
}