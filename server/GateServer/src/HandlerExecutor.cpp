#include "HandlerExecutor.h"

HandlerExecutor::HandlerExecutor(std::size_t worker_count, std::size_t queue_capacity)
	: _capacity(queue_capacity > 0 ? queue_capacity : 1)
{
	if (worker_count == 0) {
		worker_count = 1;
	}
	_workers.reserve(worker_count);
	for (std::size_t i = 0; i < worker_count; ++i) {
		// 每个工作线程进入主循环，阻塞等待任务
		_workers.emplace_back(&HandlerExecutor::RunLoop, this);
	}
}

HandlerExecutor::~HandlerExecutor()
{
	// 析构时确保排空并停止，Stop() 幂等可安全重入
	Stop();
}

bool HandlerExecutor::Post(Task task)
{
	std::lock_guard<std::mutex> lock(_mutex);
	// 停机中拒绝新任务，调用方可据此返回 503
	if (_stopping.load()) {
		return false;
	}
	// "运行中 + 排队"达到容量时拒绝入队，形成确定性反压
	if (_outstanding >= _capacity) {
		return false;
	}
	_task_que.emplace(std::move(task));
	++_outstanding;
	_cv.notify_one();
	return true;
}

void HandlerExecutor::Stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		// 仅首个调用者置位并负责回收线程，后续调用直接返回，保证幂等
		if (_stopping.exchange(true)) {
			return;
		}
	}
	// 唤醒全部工作线程；它们会继续执行队列中剩余的全部任务，直到为空才退出
	_cv.notify_all();
	for (auto& w : _workers) {
		if (w.joinable()) {
			w.join();
		}
	}
}

bool HandlerExecutor::IsStopping() const noexcept
{
	return _stopping.load();
}

void HandlerExecutor::RunLoop()
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
		// 在锁外执行任务，避免长任务阻塞 Post/Stop；
		// 防御性吞掉异常，单个任务不得击垮工作线程或破坏计数
		try {
			if (task) {
				task();
			}
		}
		catch (...) {
			// 忽略：调用方（Dispatch）已在任务闭包内处理异常并 post-back
		}
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_outstanding > 0) {
				--_outstanding;
			}
		}
	}
}
