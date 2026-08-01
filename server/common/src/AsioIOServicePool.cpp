#include "AsioIOServicePool.h"
#include <iostream>

AsioIOServicePool::AsioIOServicePool(std::size_t size)
	: _ioServices(size == 0 ? 1 : size),
	_works(size == 0 ? 1 : size),
	_nextIOService(0) {
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_works[i] = std::make_unique<Work>(boost::asio::make_work_guard(_ioServices[i]));
	}

	// start one thread per io_context
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_threads.emplace_back([this, i]() {
			_ioServices[i].run();
			});
	}
}

AsioIOServicePool::~AsioIOServicePool() {
	Stop();
	std::cout << "AsioIOServicePool destruct" << std::endl;
}

boost::asio::io_context& AsioIOServicePool::GetIOService() {
	auto& service = _ioServices[_nextIOService++];
	if (_nextIOService == _ioServices.size()) {
		_nextIOService = 0;
	}
	return service;
}

void AsioIOServicePool::Stop() {
	// 原子标志保证重复/并发调用只执行一次停止流程
	if (_stopped.exchange(true)) {
		return;
	}

	// stop contexts then release work guards so run() can exit
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_ioServices[i].stop();
		_works[i].reset();
	}

	for (auto& t : _threads) {
		if (t.joinable()) {
			t.join();
		}
	}
}
