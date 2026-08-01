#include "LogicWorker.h"

LogicWorker::LogicWorker()
{
	// 启动工作线程，进入主循环等待任务
	_work_thread = std::thread(&LogicWorker::RunLoop, this);
}

LogicWorker::~LogicWorker()
{
	// 析构时确保排空并停止，Stop() 幂等可安全重入
	Stop();
}

bool LogicWorker::Post(Task task)
{
	std::lock_guard<std::mutex> lock(_mutex);
	// 停机中拒绝新任务，调用方可据此返回 SERVER_BUSY
	if (_stopping.load()) {
		return false;
	}
	_task_que.emplace(std::move(task));
	_cv.notify_one();
	return true;
}

void LogicWorker::Stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		// 仅首个调用者置位并负责回收线程，后续调用直接返回，保证幂等
		if (_stopping.exchange(true)) {
			return;
		}
	}
	// 唤醒工作线程；工作线程会继续执行队列中剩余的全部任务，直到为空才退出
	_cv.notify_all();
	if (_work_thread.joinable()) {
		_work_thread.join();
	}
}

void LogicWorker::RunLoop()
{
	while (true) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			// 阻塞直到被停止或队列非空
			_cv.wait(lock, [this]() {
				return _stopping.load() || !_task_que.empty();
			});
			// 停机且队列已排空，工作线程安全退出
			if (_task_que.empty()) {
				return;
			}
			task = std::move(_task_que.front());
			_task_que.pop();
		}
		// 在锁外执行任务，避免长任务阻塞 Post/Stop
		task();
	}
}
