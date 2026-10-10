#include "LogicSystem.h"
#include "const.h"
#include "base64.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include "ConfigMgr.h"
#include "FileSystem.h"

using namespace std;

LogicSystem::LogicSystem(){
	for (int i = 0; i < LOGIC_WORKER_COUNT; i++) {
		_workers.push_back(std::make_shared<ResourceLogicWorker>());
	}
}

LogicSystem::~LogicSystem(){
	Stop();
}

void LogicSystem::PostMsgToQue(shared_ptr < LogicNode> msg, int index) {
	if (_workers.empty()) {
		return;
	}
	_workers[index % _workers.size()]->PostTask(msg);
}

void LogicSystem::Stop() {
	//各 ResourceLogicWorker::Stop 自身幂等（common 执行器语义），
	//静态析构与显式停机重复调用安全
	for (auto& worker : _workers) {
		if (worker) worker->Stop();
	}
}
