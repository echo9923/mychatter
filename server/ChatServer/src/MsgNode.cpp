#include "MsgNode.h"
RecvNode::RecvNode(short max_len, short msg_type):MsgNode(max_len),
_msg_type(msg_type){

}


SendNode::SendNode(const char* msg, short max_len, short msg_type):MsgNode(max_len + HEAD_TOTAL_LEN)
, _msg_type(msg_type){
	//先发送消息类型, 转为网络字节序
	short msg_type_host = boost::asio::detail::socket_ops::host_to_network_short(msg_type);
	memcpy(_data, &msg_type_host, HEAD_TYPE_LEN);
	//转为网络字节序
	short max_len_host = boost::asio::detail::socket_ops::host_to_network_short(max_len);
	memcpy(_data + HEAD_TYPE_LEN, &max_len_host, HEAD_DATA_LEN);
	memcpy(_data + HEAD_TYPE_LEN + HEAD_DATA_LEN, msg, max_len);
}
