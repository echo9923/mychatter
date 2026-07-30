#pragma once
#include <string>
#include "const.h"
#include <iostream>
#include <boost/asio.hpp>
using namespace std;
using boost::asio::ip::tcp;
class LogicSystem;

/**
 * @brief 消息节点基类
 * 
 * TCP消息的内存管理基类，维护一块动态分配的缓冲区。
 * 作为 RecvNode（接收节点）和 SendNode（发送节点）的父类，
 * 提供统一的缓冲区分配、释放和清空操作。
 */
class MsgNode
{
public:
	/**
	 * @brief 构造函数，分配指定大小的数据缓冲区
	 * @param max_len 缓冲区最大字节数
	 */
	MsgNode(short max_len) :_total_len(max_len), _cur_len(0) {
		_data = new char[_total_len + 1]();
		_data[_total_len] = '\0';
	}

	/// 析构函数，释放数据缓冲区内存
	~MsgNode() {
		std::cout << "destruct MsgNode" << endl;
		delete[] _data;
	}

	/// 清空缓冲区数据并重置当前长度为0
	void Clear() {
		::memset(_data, 0, _total_len);
		_cur_len = 0;
	}

	short _cur_len;    ///< 当前已填充的数据长度（字节）
	short _total_len;  ///< 缓冲区总容量（字节）
	char* _data;       ///< 数据缓冲区指针，存储实际消息内容
};

/**
 * @brief 接收消息节点
 * 
 * 存储从客户端接收到的一条完整消息，包含消息ID和数据体。
 * IO线程解析完消息后创建RecvNode，封装到LogicNode中投递给业务线程处理。
 */
class RecvNode :public MsgNode {
	friend class LogicSystem;
public:
	/**
	 * @brief 构造接收节点
	 * @param max_len 消息体最大长度
	 * @param msg_id 消息类型ID（用于业务分发）
	 */
	RecvNode(short max_len, short msg_id);

	/**
	 * @brief 获取消息类型ID（供 IO 线程做路由分片绑定，计划1.3）
	 * @return 消息类型ID
	 */
	short GetMsgId() const { return _msg_id; }
private:
	short _msg_id;  ///< 消息类型ID，用于确定业务处理逻辑
};

/**
 * @brief 发送消息节点
 * 
 * 存储待发送给客户端的一条完整消息，包含消息头（ID+长度）和数据体。
 * 发送时将其放入会话的发送队列，由异步写入操作逐步发送。
 */
class SendNode:public MsgNode {
	friend class LogicSystem;
public:
	/**
	 * @brief 构造发送节点，将消息内容拷贝到内部缓冲区
	 * @param msg 消息数据指针
	 * @param max_len 消息数据长度
	 * @param msg_id 消息类型ID
	 */
	SendNode(const char* msg,short max_len, short msg_id);
private:
	short _msg_id;  ///< 消息类型ID，写入消息头部发送给客户端
};

