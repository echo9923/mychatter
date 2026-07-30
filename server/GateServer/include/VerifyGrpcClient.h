#pragma once
#include "ConfigMgr.h"
#include "Singleton.h"
#include "const.h"
#include "verify.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using message::GetVarifyReq;
using message::GetVarifyRsp;
using message::VarifyService;

class VerifyGrpcClient : public Singleton<VerifyGrpcClient> {
    friend class Singleton<VerifyGrpcClient>;

  public:
    ~VerifyGrpcClient() {
    }
    GetVarifyRsp GetVarifyCode(std::string email) {
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        GetVarifyRsp reply;
        GetVarifyReq request;
        request.set_email(email);
        auto stub = VarifyService::NewStub(channel_);
        Status status = stub->GetVarifyCode(&context, request, &reply);

        if (status.ok()) {
            return reply;
        } else {
            reply.set_error(ErrorCodes::RPCFailed);
            return reply;
        }
    }

  private:
    VerifyGrpcClient();

    std::shared_ptr<Channel> channel_;
};
