#pragma once
#include "Singleton.h"
#include <queue>
#include <thread>
#include "CSession.h"
#include <queue>
#include <map>
#include <functional>
#include "const.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <unordered_map>
#include "LogicWorker.h"
#include "FileInfo.h"


typedef  function<void(shared_ptr<CSession>, const short &msg_type, const string &msg_data)> FunCallBack;
class LogicSystem:public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem();
	void PostMsgToQue(shared_ptr < LogicNode> msg, int index);
private:
	LogicSystem();
	std::vector<std::shared_ptr<LogicWorker> > _workers;
};

