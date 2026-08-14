#pragma once
#include "Singleton.h"
#include "FileWorker.h"
#include <memory>
#include <vector>

class FileSystem :public Singleton<FileSystem>
{
	friend class Singleton<FileSystem>;
public:
	~FileSystem();
	void PostMsgToQue(shared_ptr <FileTask> msg, int index);
	void PostDownloadTaskToQue(std::shared_ptr<DownloadTask> msg, int index);
	/// 资源分片上传（1037）：index 必须由 ResourceWorkerIndex(message_id, FILE_WORKER_COUNT) 计算
	void PostChunkToQue(std::shared_ptr<ResourceChunkTask> msg, int index);
	/// 资源分片下载（1047）：index 必须由 ResourceWorkerIndex(message_id, DOWN_LOAD_WORKER_COUNT) 计算
	void PostChunkDownToQue(std::shared_ptr<ResourceChunkDownTask> msg, int index);
	/// 任意闭包到指定 FileWorker（1041 进度查询等，与分片写入同 worker 串行化）
	void PostClosureToQue(std::function<void()> fn, int index);
private:
	FileSystem();
	std::vector<std::shared_ptr<FileWorker>>  _file_workers;
	std::vector<std::shared_ptr<DownloadWorker>> _down_load_worker;
};


