#include "ChatServiceImpl.h"

#include "CSession.h"
#include "LogicSystem.h"
#include "MysqlMgr.h"
#include "UserMgr.h"
#include "const.h"

#include <iostream>

grpc::Status ChatServiceImpl::NotifyUserMessage(
	grpc::ServerContext*, const message::NotifyUserMessageReq* request,
	message::NotifyUserMessageRsp* response)
{
	response->set_message_id(request->message_id());
	response->set_friend_request_id(request->friend_request_id());
	const int uid = request->to_uid();
	if (!UserMgr::GetInstance()->GetSession(uid)) {
		response->set_error(ErrorCodes::RECIPIENT_OFFLINE);
		return grpc::Status::OK;
	}

	const int event_type = request->event_type();
	const std::int64_t message_id = request->message_id();
	const std::int64_t friend_request_id = request->friend_request_id();
	if (!LogicSystem::GetInstance()->PostToUser(uid,
		[uid, event_type, message_id, friend_request_id]() {
		auto session = UserMgr::GetInstance()->GetSession(uid);
		if (!session) return;
		UserEvent event;
		if (!MysqlMgr::GetInstance()->GetUserEvent(uid, event_type, message_id,
			friend_request_id, event)) {
			std::cerr << "NotifyUserMessage: invalid event for message " << message_id
				<< " for uid " << uid << std::endl;
			return;
		}
		auto envelope = LogicSystem::BuildUserEventEnvelope(event, uid);
		envelope["error"] = ErrorCodes::Success;
		session->Send(envelope.dump(), ID_NOTIFY_USER_MESSAGE);
	})) {
		response->set_error(ErrorCodes::SERVER_BUSY);
		return grpc::Status::OK;
	}

	response->set_error(ErrorCodes::Success);
	return grpc::Status::OK;
}

grpc::Status ChatServiceImpl::NotifyKickUser(
	grpc::ServerContext*, const message::KickUserReq* request,
	message::KickUserRsp* response)
{
	const int uid = request->uid();
	response->set_uid(uid);
	if (!UserMgr::GetInstance()->GetSession(uid)) {
		response->set_error(ErrorCodes::RECIPIENT_OFFLINE);
		return grpc::Status::OK;
	}
	if (!LogicSystem::GetInstance()->PostToUser(uid, [uid]() {
		auto session = UserMgr::GetInstance()->GetSession(uid);
		if (session) session->NotifyOffline(uid);
	})) {
		response->set_error(ErrorCodes::SERVER_BUSY);
		return grpc::Status::OK;
	}
	response->set_error(ErrorCodes::Success);
	return grpc::Status::OK;
}
