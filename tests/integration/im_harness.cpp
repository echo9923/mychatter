// im_harness.cpp — Win32 process management + INI generation.
#include "im_harness.h"
#include "im_common.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef IM_REPO_ROOT
#define IM_REPO_ROOT "."
#endif
#ifndef IM_BUILD_CONFIG
#define IM_BUILD_CONFIG "Debug"
#endif

#include <boost/asio.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace imt {

// ---------------------------------------------------------------------------
// Exe path resolution
// ---------------------------------------------------------------------------
std::string ExeRef::AbsPath() const {
	return std::string(IM_REPO_ROOT) + "/out/run/" IM_BUILD_CONFIG "/"
		+ subdir + "/" + exe_name;
}

// ---------------------------------------------------------------------------
// ProcessManager
// ---------------------------------------------------------------------------
ProcessManager::ProcessManager() {}
ProcessManager::~ProcessManager() { StopAll(); }

static bool DirExists(const std::string& p) {
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool MakeDirDeep(const std::string& p) {
	if (DirExists(p)) return true;
	// Recursively create via SHCreateDirectoryEx-free manual walk.
	std::string acc;
	for (std::size_t i = 0; i < p.size(); ++i) {
		const char c = p[i];
		acc.push_back((c == '/') ? '\\' : c);
		if (c == '/' || c == '\\' || i + 1 == p.size()) {
			if (!acc.empty()) {
				CreateDirectoryA(acc.c_str(), nullptr);  // ignore if exists
			}
		}
	}
	return DirExists(p);
}

bool ProcessManager::InitBaseDir() {
	if (inited_) return true;
	char tmp[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, tmp);
	if (n == 0 || n > MAX_PATH) return false;
	// Unique subdir per run.
	base_dir_ = std::string(tmp) + "imtest-" +
		std::to_string(GetCurrentProcessId()) + "-" +
		std::to_string(static_cast<long long>(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	if (!MakeDirDeep(base_dir_)) return false;
	inited_ = true;
	return true;
}

static bool WriteFileAtomic(const std::string& path, const std::string& content) {
	HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	BOOL ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
	CloseHandle(h);
	return ok && written == content.size();
}

bool ProcessManager::Start(const ServiceSpec& spec, int ready_timeout_ms) {
	if (!InitBaseDir()) {
		std::printf("[harness] InitBaseDir failed\n");
		return false;
	}
	const std::string work = base_dir_ + "\\" + spec.name;
	if (!MakeDirDeep(work)) {
		std::printf("[harness] cannot create work dir %s\n", work.c_str());
		return false;
	}
	const std::string cfg_path = work + "\\config.ini";
	if (!WriteFileAtomic(cfg_path, spec.config_ini)) {
		std::printf("[harness] cannot write %s\n", cfg_path.c_str());
		return false;
	}
	const std::string log_path = work + "\\run.log";
	const std::string exe_path = spec.exe.AbsPath();

	HANDLE hLog = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hLog == INVALID_HANDLE_VALUE) {
		std::printf("[harness] cannot open log %s\n", log_path.c_str());
		return false;
	}

	// Job Object: KILL_ON_JOB_CLOSE ensures the whole tree dies on StopAll().
	HANDLE hJob = CreateJobObjectA(nullptr, nullptr);
	if (!hJob) { CloseHandle(hLog); return false; }
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji{};
	ji.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &ji, sizeof(ji));

	// Build the command line: <exe>  (servers take no args; config is via cwd).
	STARTUPINFOA si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput  = nullptr;
	si.hStdOutput = hLog;
	si.hStdError  = hLog;
	PROCESS_INFORMATION pi{};
	std::string cmd = exe_path;

	// Create suspended so we can assign to the job before any thread runs.
	BOOL ok = CreateProcessA(nullptr,            // lpApplicationName
		const_cast<LPSTR>(cmd.c_str()),  // lpCommandLine (mutable)
		nullptr, nullptr,
		TRUE,                             // bInheritHandles (for log)
		CREATE_SUSPENDED | CREATE_NO_WINDOW,
		nullptr,
		work.c_str(),                     // lpCurrentDirectory = work dir
		&si, &pi);
	if (!ok) {
		DWORD err = GetLastError();
		std::printf("[harness] CreateProcess failed for %s (err=%lu)\n",
			exe_path.c_str(), err);
		CloseHandle(hLog); CloseHandle(hJob);
		return false;
	}
	AssignProcessToJobObject(hJob, pi.hProcess);
	ResumeThread(pi.hThread);

	// We can close the log handle now; the child has its own inherited copy.
	CloseHandle(hLog);

	Proc proc;
	proc.name     = spec.name;
	proc.hProcess = pi.hProcess;
	proc.hThread  = pi.hThread;
	proc.hJob     = hJob;
	procs_.push_back(proc);

	std::printf("[harness] started %s (pid=%lu), cwd=%s, ready_port=%u, log=%s\n",
		spec.name.c_str(), pi.dwProcessId, work.c_str(),
		spec.ready_port, log_path.c_str());

	if (spec.ready_port != 0) {
		if (!WaitForPort("127.0.0.1", spec.ready_port, ready_timeout_ms)) {
			std::printf("[harness] %s did not become ready on port %u within %d ms\n",
				spec.name.c_str(), spec.ready_port, ready_timeout_ms);
			return false;
		}
	}
	return true;
}

bool ProcessManager::WaitForPort(const std::string& host, unsigned short port, int timeout_ms) {
	namespace asio = boost::asio;
	using boost::asio::ip::tcp;
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		try {
			asio::io_context ioc;
			tcp::socket s(ioc);
			tcp::resolver r(ioc);
			auto eps = r.resolve(host, std::to_string(port));
			asio::connect(s, eps);
			boost::system::error_code ign;
			s.close(ign);
			return true;
		} catch (...) {
			std::this_thread::sleep_for(std::chrono::milliseconds(75));
		}
	}
	return false;
}

void ProcessManager::StopAll() {
	for (auto& p : procs_) {
		if (p.hProcess) {
			TerminateProcess(p.hProcess, 1);
		}
	}
	for (auto& p : procs_) {
		if (p.hJob)     { CloseHandle(p.hJob);     p.hJob = nullptr; }
		if (p.hThread)  { CloseHandle(p.hThread);  p.hThread = nullptr; }
		if (p.hProcess) { CloseHandle(p.hProcess); p.hProcess = nullptr; }
	}
	procs_.clear();
}

bool ProcessManager::StopOne(const std::string& name) {
	for (auto& p : procs_) {
		if (p.name == name && p.hProcess) {
			TerminateProcess(p.hProcess, 1);
			WaitForSingleObject(p.hProcess, 3000);
			if (p.hJob)     { CloseHandle(p.hJob);     p.hJob = nullptr; }
			if (p.hThread)  { CloseHandle(p.hThread);  p.hThread = nullptr; }
			if (p.hProcess) { CloseHandle(p.hProcess); p.hProcess = nullptr; }
			return true;
		}
	}
	return false;
}

bool ProcessManager::IsRunning(const std::string& name) {
	for (auto& p : procs_) {
		if (p.name == name && p.hProcess) {
			DWORD exit_code = 0;
			if (GetExitCodeProcess(p.hProcess, &exit_code)) {
				return exit_code == STILL_ACTIVE;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// INI generators
//
// MYSQL_HOST/REDIS_HOST/etc. are constexpr const char* variables (not macros),
// so they cannot be placed adjacent to string literals; concatenate via
// std::string. Values mirror the production INI layout, retargeted to the
// harness's dedicated ports.
// ---------------------------------------------------------------------------
static const std::string& MHost() { static const std::string s = MYSQL_HOST; return s; }
static const std::string& MUser() { static const std::string s = MYSQL_USER; return s; }
static const std::string& MPwd()  { static const std::string s = MYSQL_PASSWD; return s; }
static const std::string& MSch()  { static const std::string s = MYSQL_SCHEMA; return s; }
static const std::string& RHost() { static const std::string s = REDIS_HOST; return s; }
static const std::string& RPwd()  { static const std::string s = REDIS_PASSWD; return s; }

static std::string MysqlBlock() {
	std::string s;
	s += "[Mysql]\nHost = " + MHost() + "\nPort = " + std::to_string(MYSQL_PORT)
		+ "\nUser = " + MUser() + "\nPasswd = " + MPwd() + "\nSchema = " + MSch() + "\n";
	return s;
}
static std::string RedisBlock() {
	std::string s;
	s += "[Redis]\nHost = " + RHost() + "\nPort = " + std::to_string(REDIS_PORT)
		+ "\nPasswd = " + RPwd() + "\n";
	return s;
}

std::string MakeStatusIni() {
	std::string s;
	s += "[StatusServer]\n";
	s += "Port = " + std::to_string(STATUS_GRPC_PORT) + "\n";
	s += "Host = 0.0.0.0\n";
	s += MysqlBlock();
	s += RedisBlock();
	s += "[chatservers]\nName = chatserver1,chatserver2\n";
	s += "[chatserver1]\nName = chatserver1\nHost = 127.0.0.1\nPort = "
		+ std::to_string(CHAT1_TCP_PORT) + "\n";
	s += "[chatserver2]\nName = chatserver2\nHost = 127.0.0.1\nPort = "
		+ std::to_string(CHAT2_TCP_PORT) + "\n";
	return s;
}

std::string MakeChatIni(const std::string& self_name, unsigned short tcp_port,
                        unsigned short rpc_port, int logic_workers) {
	return MakeChatIniPeer(self_name, tcp_port, rpc_port, logic_workers,
	                       "", 0, 0);
}

std::string MakeChatIniPeer(const std::string& self_name, unsigned short tcp_port,
                            unsigned short rpc_port, int logic_workers,
                            const std::string& peer_name,
                            unsigned short peer_tcp_port,
                            unsigned short peer_rpc_port) {
	std::string s;
	s += "[GateServer]\nPort = " + std::to_string(GATE_HTTP_PORT) + "\n";
	s += "[VarifyServer]\nHost = 127.0.0.1\nPort = 50051\n";
	s += "[StatusServer]\nHost = 127.0.0.1\nPort = "
		+ std::to_string(STATUS_GRPC_PORT) + "\n";
	s += "[SelfServer]\nName = " + self_name + "\nHost = 0.0.0.0\nPort  = "
		+ std::to_string(tcp_port) + "\nRPCPort = " + std::to_string(rpc_port) + "\n";
	s += MysqlBlock();
	s += RedisBlock();
	if (peer_name.empty()) {
		s += "[PeerServer]\nServers =\n";
	} else {
		s += "[PeerServer]\nServers = " + peer_name + "\n";
		s += "[" + peer_name + "]\nName = " + peer_name + "\nHost = 127.0.0.1\nPort = "
			+ std::to_string(peer_rpc_port) + "\n";
	}
	s += "[Concurrency]\nLogicWorkers = " + std::to_string(logic_workers)
		+ "\nDeliveryWorkers = 4\n";
	s += "[Delivery]\nOfflineTtlSeconds = 604800\nOfflinePullBatch = 100\n";
	s += "PullMaxBytes = 30000\nRpcDeadlineMs = 3000\nRpcMaxAttempts = 3\nRpcBackoffMs = 100\n";
	return s;
}

std::string MakeGateIni() {
	std::string s;
	s += "[GateServer]\nPort = " + std::to_string(GATE_HTTP_PORT) + "\n";
	s += "[VarifyServer]\nHost = 127.0.0.1\nPort = 50051\n";
	s += "[StatusServer]\nHost = 127.0.0.1\nPort = "
		+ std::to_string(STATUS_GRPC_PORT) + "\n";
	s += MysqlBlock();
	s += RedisBlock();
	s += "[ResServer]\nName = reserver\nHost = 127.0.0.1\nPort = "
		+ std::to_string(RESOURCE_HTTP_PORT) + "\n";
	s += "[Concurrency]\nHandlerWorkers = 4\nHandlerQueueCapacity = 1024\n";
	return s;
}

std::string MakeResourceIni() {
	std::string s;
	s += "[SelfServer]\nName = reserver\nHost = 0.0.0.0\nPort = "
		+ std::to_string(RESOURCE_HTTP_PORT) + "\n";
	s += MysqlBlock();
	s += RedisBlock();
	s += "[Output]\nPath = bin\n";
	s += "[Static]\nPath = static\n";
	s += "[chatserver1]\nName = chatserver1\nHost = 127.0.0.1\nPort = "
		+ std::to_string(CHAT1_GRPC_PORT) + "\n";
	s += "[chatserver2]\nName = chatserver2\nHost = 127.0.0.1\nPort = "
		+ std::to_string(CHAT2_GRPC_PORT) + "\n";
	s += "[Delivery]\nOfflineTtlSeconds = 604800\nOfflinePullBatch = 100\n";
	s += "PullMaxBytes = 30000\nRpcDeadlineMs = 3000\nRpcMaxAttempts = 3\nRpcBackoffMs = 100\n";
	return s;
}

} // namespace imt
