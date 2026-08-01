#pragma once

#include <functional>

// Defer类：RAII 延迟执行器，析构时执行构造传入的回调。
class Defer {
public:
	// 接受一个lambda表达式或者函数指针
	Defer(std::function<void()> func) : func_(func) {}

	// 析构函数中执行传入的函数
	~Defer() {
		func_();
	}

private:
	std::function<void()> func_;
};
