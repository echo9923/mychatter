// im_harness.h — process management + INI generation for IM integration tests.
//
// Each server (StatusServer / ChatServer / GateServer / ResourceServer) reads
// its config.ini from the *current working directory* (see ConfigMgr.cpp), so
// the harness launches every server with cwd = a temp dir holding a generated
// config.ini that points at the test ports (18080/18090/18091/15052/...). A
// Win32 Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantees the whole
// process tree is torn down on StopAll()/destruction, even if a handler wedges.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace imt {

// Absolute path to a built server executable. Resolved at runtime from
// IM_REPO_ROOT / IM_BUILD_CONFIG (compile definitions) unless overridden.
struct ExeRef {
	std::string subdir;   // e.g. "chatserver1", "StatusServer", "GateServer"
	std::string exe_name; // e.g. "ChatServer.exe"
	std::string AbsPath() const;
};

// A server the harness will launch.
struct ServiceSpec {
	std::string name;        // human label / temp subdir name
	ExeRef      exe;
	std::string config_ini;  // full INI body, written to <work>/config.ini
	unsigned short ready_port = 0;  // TCP port polled for readiness
};

class ProcessManager {
public:
	ProcessManager();
	~ProcessManager();
	ProcessManager(const ProcessManager&) = delete;
	ProcessManager& operator=(const ProcessManager&) = delete;

	// Create the harness base temp dir (unique per run). Idempotent.
	bool InitBaseDir();

	// Start a service: writes config.ini, launches the exe with cwd=work dir,
	// then polls ready_port until it accepts a TCP connection. Returns false on
	// launch failure or readiness timeout.
	bool Start(const ServiceSpec& spec, int ready_timeout_ms);

	// Poll a TCP port for readiness (used both internally and by scenarios).
	static bool WaitForPort(const std::string& host, unsigned short port, int timeout_ms);

	// Terminate every started process and free handles. Safe to call repeatedly.
	void StopAll();

	// Terminate a single process by its spec.name (the work-dir label).
	// Returns true if a matching process was found and terminated.
	// The handle is freed; StopAll() will skip it.
	bool StopOne(const std::string& name);

	// Check whether a process with the given name is still running.
	bool IsRunning(const std::string& name);

	const std::string& base_dir() const { return base_dir_; }

private:
	struct Proc {
		std::string name;
		void* hProcess = nullptr;
		void* hThread  = nullptr;
		void* hJob     = nullptr;
	};
	std::vector<Proc> procs_;
	std::string       base_dir_;
	bool              inited_ = false;
};

// ---- INI generators (mirror production layout, retargeted to test ports) ---
std::string MakeStatusIni();
std::string MakeChatIni(const std::string& self_name, unsigned short tcp_port,
                        unsigned short rpc_port, int logic_workers);
std::string MakeChatIniPeer(const std::string& self_name, unsigned short tcp_port,
                            unsigned short rpc_port, int logic_workers,
                            const std::string& peer_name,
                            unsigned short peer_tcp_port,
                            unsigned short peer_rpc_port);
std::string MakeGateIni();
std::string MakeResourceIni();

} // namespace imt
