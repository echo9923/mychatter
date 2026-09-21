#include "FileSystem.h"
#include "const.h"

FileSystem::~FileSystem()
{

}

void FileSystem::PostMsgToQue(shared_ptr<FileTask> msg, int index)
{
	_file_workers[index]->PostTask(msg);
}

void FileSystem::PostDownloadTaskToQue(std::shared_ptr<DownloadTask> msg, int index)
{
	_down_load_worker[index]->PostTask(msg);
}

void FileSystem::PostChunkToQue(std::shared_ptr<ResourceChunkTask> msg, int index)
{
	_file_workers[index]->PostChunkTask(msg);
}

void FileSystem::PostChunkDownToQue(std::shared_ptr<ResourceChunkDownTask> msg, int index)
{
	_down_load_worker[index]->PostChunkTask(msg);
}

void FileSystem::PostClosureToQue(std::function<void(FileWorker&)> fn, int index)
{
	_file_workers[index]->PostClosure(std::move(fn));
}

FileSystem::FileSystem()
{
	for (int i = 0; i < FILE_WORKER_COUNT; i++) {
		_file_workers.push_back(std::make_shared<FileWorker>());
	}

	for (int i = 0; i < DOWN_LOAD_WORKER_COUNT; i++) {
		_down_load_worker.push_back(std::make_shared<DownloadWorker>());
	}
}
