// StatusServer.cpp : 此文件包含 "main" 函数。程序执行将在此开始并结束。
//

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <string>
#include "const.h"
#include "ConfigMgr.h"
#include <hiredis/hiredis.h>
#include "RedisMgr.h"
#include <memory>
#include <thread>
#include <boost/asio.hpp>
#include "StatusServiceImpl.h"
#include "SecurityUtil.h"

/// Read an entire file into a std::string. Returns empty string on failure.
static std::string ReadFileContent(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		return std::string();
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/// Fetch a required environment variable; prints the variable name and returns
/// empty string when missing/empty (caller treats as fatal).
static std::string RequireEnv(const char* name) {
	const char* val = std::getenv(name);
	if (val == nullptr || val[0] == '\0') {
		std::cerr << "Missing required environment variable: " << name << std::endl;
		return std::string();
	}
	return std::string(val);
}

/// One-shot migration: acquire a short-lived lock, SCAN+DEL all legacy utoken_*
/// keys, then stamp auth:schema=v2.  Returns false on any Redis failure so
/// the server refuses to start with a half-migrated schema.
static bool MigrateAuthTokenSchema() {
	const std::string lock_key = "auth:migrate:lock";
	const std::string schema_key = "auth:schema";
	const std::string legacy_pattern = "utoken_*";

	// 1. Acquire migration lock (SET NX EX 60), retry up to ~30s.
	std::string lock_token = security::GenerateSessionToken();
	if (lock_token.empty()) {
		std::cerr << "Migration failed: cannot generate lock token" << std::endl;
		return false;
	}

	bool locked = false;
	for (int attempt = 0; attempt < 30; ++attempt) {
		if (RedisMgr::GetInstance()->SetNx(lock_key, lock_token, 60)) {
			locked = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	if (!locked) {
		std::cerr << "Migration failed: could not acquire migration lock within 30s" << std::endl;
		return false;
	}

	// Ensure lock is always released.
	Defer release([&lock_key, &lock_token]() {
		// Only delete if we still own the lock (compare-and-delete via Lua).
		std::string lua =
			"if redis.call('get',KEYS[1])==ARGV[1] then "
			"return redis.call('del',KEYS[1]) else return 0 end";
		RedisMgr::GetInstance()->Eval(lua, { lock_key }, { lock_token });
	});

	// 2. SCAN all legacy utoken_* keys.
	auto keys = RedisMgr::GetInstance()->Scan(legacy_pattern);

	// 3. Delete each key individually (DEL supports one-at-a-time).
	for (const auto& key : keys) {
		if (!RedisMgr::GetInstance()->Del(key)) {
			std::cerr << "Migration failed: cannot delete legacy key" << std::endl;
			return false;
		}
	}

	if (!keys.empty()) {
		std::cout << "Migration: deleted " << keys.size() << " legacy auth keys" << std::endl;
	}

	// 4. Stamp schema version.
	if (!RedisMgr::GetInstance()->Set(schema_key, "v2")) {
		std::cerr << "Migration failed: cannot set schema version" << std::endl;
		return false;
	}

	std::cout << "Migration: auth schema v2 ready" << std::endl;
	return true;
}

bool RunServer() {
	auto& cfg = ConfigMgr::Inst();

	// --- Load mTLS certificates from environment (fail-fast) ---
	std::string ca_path = RequireEnv("LLFC_STATUS_CA_CERT_PATH");
	if (ca_path.empty()) return false;
	std::string cert_path = RequireEnv("LLFC_STATUS_SERVER_CERT_PATH");
	if (cert_path.empty()) return false;
	std::string key_path = RequireEnv("LLFC_STATUS_SERVER_KEY_PATH");
	if (key_path.empty()) return false;

	std::string ca_pem = ReadFileContent(ca_path);
	if (ca_pem.empty()) {
		std::cerr << "Cannot read CA cert: " << ca_path << std::endl;
		return false;
	}
	std::string cert_pem = ReadFileContent(cert_path);
	if (cert_pem.empty()) {
		std::cerr << "Cannot read server cert: " << cert_path << std::endl;
		return false;
	}
	std::string key_pem = ReadFileContent(key_path);
	if (key_pem.empty()) {
		std::cerr << "Cannot read server key: " << key_path << std::endl;
		return false;
	}

	// --- Schema migration: must succeed before serving any request ---
	if (!MigrateAuthTokenSchema()) {
		return false;
	}

	std::string server_address(cfg["StatusServer"]["Host"] + ":" + cfg["StatusServer"]["Port"]);
	StatusServiceImpl service;

	// --- Configure mutual TLS server credentials ---
	grpc::SslServerCredentialsOptions ssl_opts(
		GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
	ssl_opts.pem_root_certs = ca_pem;
	grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert;
	key_cert.private_key = key_pem;
	key_cert.cert_chain = cert_pem;
	ssl_opts.pem_key_cert_pairs.push_back(key_cert);

	auto creds = grpc::SslServerCredentials(ssl_opts);

	grpc::ServerBuilder builder;
	builder.AddListeningPort(server_address, creds);
	builder.RegisterService(&service);

	// 构建并启动gRPC服务器
	std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
	std::cout << "Server listening on " << server_address << " (mTLS)" << std::endl;

	// 创建Boost.Asio的io_context
	boost::asio::io_context io_context;
	// 创建signal_set用于捕获SIGINT
	boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);

	// 设置异步等待SIGINT信号
	signals.async_wait([&server, &io_context](const boost::system::error_code& error, int signal_number) {
		if (!error) {
			std::cout << "Shutting down server..." << std::endl;
			server->Shutdown(); // 优雅地关闭服务器
			io_context.stop(); // 停止io_context
		}
		});

	// 在单独的线程中运行io_context
	std::thread([&io_context]() { io_context.run(); }).detach();

	// 等待服务器关闭
	server->Wait();
	return true;
}

int main(int argc, char** argv) {
	try {
		bool ok = RunServer();
		RedisMgr::GetInstance()->Close();
		if (!ok) {
			return EXIT_FAILURE;
		}
	}
	catch (std::exception const& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		RedisMgr::GetInstance()->Close();
		return EXIT_FAILURE;
	}

	return 0;
}
