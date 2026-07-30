#pragma once
#include <memory>
#include <mutex>
#include <iostream>
using namespace std;

/**
 * @brief 通用单例模式基类模板
 * 
 * 提供线程安全的单例实例管理，子类通过继承并声明 friend 来使用。
 * 使用 std::call_once 保证实例只创建一次（线程安全）。
 * 实例以 shared_ptr 管理，程序结束时自动释放。
 * 
 * 使用方式：
 * @code
 * class MyClass : public Singleton<MyClass> {
 *     friend class Singleton<MyClass>;
 * private:
 *     MyClass(); // 构造函数私有化
 * };
 * // 获取实例: MyClass::GetInstance()
 * @endcode
 */
template <typename T>
class Singleton {
protected:
	/// 默认构造函数（protected，仅子类可调用）
	Singleton() = default;
	/// 禁止拷贝构造
	Singleton(const Singleton<T>&) = delete;
	/// 禁止拷贝赋值
	Singleton& operator=(const Singleton<T>& st) = delete;
	
	/// 单例实例的智能指针（静态成员，全局唯一）
	static std::shared_ptr<T> _instance;

public:
	/**
	 * @brief 获取单例实例（线程安全，首次调用时创建）
	 * @return 单例实例的shared_ptr
	 */
	static std::shared_ptr<T> GetInstance() {
		static std::once_flag s_flag;
		std::call_once(s_flag, [&]() {
			_instance = shared_ptr<T>(new T);
			});

		return _instance;
	}

	/// 打印单例实例的内存地址（调试用）
	void PrintAddress() {
		std::cout << _instance.get() << endl;
	}

	/// 析构函数
	~Singleton() {
		std::cout << "this is singleton destruct" << std::endl;
	}
};

/// 静态成员变量定义（每个模板实例化类型对应一个独立的_instance）
template <typename T>
std::shared_ptr<T> Singleton<T>::_instance = nullptr;
