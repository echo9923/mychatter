/**
 * @file LogicSystem.cpp
 * @brief 网关服务器(GateServer)的业务逻辑处理系统实现
 * 
 * 本文件实现了GateServer的核心路由注册与请求分发逻辑。
 * GateServer作为整个微服务架构的入口网关，负责：
 * 1. 接收客户端的HTTP请求（GET/POST）
 * 2. 根据URL路径将请求分发到对应的处理函数
 * 3. 通过gRPC调用后端微服务（验证码服务、状态服务）
 * 4. 通过Redis进行验证码缓存管理
 * 5. 通过MySQL进行用户数据的增删改查
 * 
 * 注册的路由包括：
 * - GET  /get_test       : 测试接口，回显GET参数
 * - POST /test_procedure : 测试MySQL存储过程调用
 * - POST /get_varifycode : 获取邮箱验证码
 * - POST /user_register  : 用户注册
 * - POST /reset_pwd      : 重置密码
 * - POST /user_login     : 用户登录
 */

#include "LogicSystem.h"       // 逻辑系统头文件，定义了路由注册和请求分发接口
#include "HttpConnection.h"    // HTTP连接封装，包含请求和响应对象
#include "HandlerExecutor.h"   // 有界 handler worker 池（计划2.1）
#include "VerifyGrpcClient.h"  // 验证码服务的gRPC客户端（调用VarifyServer）
#include "RedisMgr.h"          // Redis管理器，用于验证码的缓存与校验
#include "MysqlMgr.h"          // MySQL管理器，用于用户数据的持久化操作
#include "StatusGrpcClient.h"  // 状态服务的gRPC客户端（调用StatusServer分配ChatServer）
#include "ConfigMgr.h"         // 配置读取，用于解析 [Concurrency] worker 数与队列容量

/**
 * @brief 从 [Concurrency] 读取一个正整数配置项，缺失/非数字/<=0 时回退
 * @param key      配置键名（如 "HandlerWorkers"）
 * @param fallback 回退默认值
 * @return 解析到的正整数，或 fallback
 */
static std::size_t ReadConcurrencySize(const std::string& key, std::size_t fallback) {
	auto& cfg = ConfigMgr::Inst();
	std::string val = cfg.GetValue("Concurrency", key);
	if (val.empty()) {
		return fallback;
	}
	try {
		std::size_t pos = 0;
		long long n = std::stoll(val, &pos);
		//必须整串消费（允许前导空白），否则视为非法值回退（计划2.5）
		if (pos != val.size() || n <= 0) {
			return fallback;
		}
		return static_cast<std::size_t>(n);
	}
	catch (...) {
		return fallback;
	}
}

/**
 * @brief LogicSystem构造函数 —— 注册所有HTTP路由及对应的处理逻辑
 * 
 * 在构造时通过RegGet/RegPost方法将URL路径与处理函数（lambda）绑定。
 * 当HTTP请求到达时，HttpConnection会根据请求方法和路径调用对应的handler。
 * 每个handler接收一个shared_ptr<HttpConnection>参数，通过它读取请求、写入响应。
 */
LogicSystem::LogicSystem() {
	// ==================== GET /get_test ====================
	// 测试接口：接收GET请求，遍历所有URL查询参数并回显到响应体中
	// 用途：验证HTTP GET请求的参数解析功能是否正常
	RegGet("/get_test", [](std::shared_ptr<HttpConnection> connection) {
		// 向响应体写入提示信息
		beast::ostream(connection->_response.body()) << "receive get_test req " << std::endl;
		// 遍历connection中解析出的所有GET查询参数（key-value键值对）
		int i = 0;
		for (auto& elem : connection->_get_params) {
			i++;
			// 依次输出每个参数的键和值
			beast::ostream(connection->_response.body()) << "param" << i << " key is " << elem.first;
			beast::ostream(connection->_response.body()) << ", " <<  " value is " << elem.second << std::endl;
		}

		// 设置响应头的Content-Type为纯文本格式
		connection->_response.set(http::field::content_type, "text/plain");
	});

	// ==================== POST /test_procedure ====================
	// 测试接口：验证MySQL存储过程的调用功能
	// 请求体JSON格式: {"email": "xxx@xxx.com"}
	// 响应体JSON格式: {"error": 0, "email": "...", "name": "...", "uid": 123}
	RegPost("/test_procedure", [](std::shared_ptr<HttpConnection> connection) {
		// 从请求体中提取原始字符串数据
		auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive body is " << body_str << std::endl;
		// 设置响应Content-Type为JSON格式
		connection->_response.set(http::field::content_type, "text/json");
		json root;
		// 解析请求体中的JSON字符串
		auto src_root = json::parse(body_str, nullptr, false);
		if (src_root.is_discarded()) {
			// JSON解析失败，返回错误码
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 检查请求JSON中是否包含必需的"email"字段
		if (!src_root.contains("email")) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 提取邮箱地址，调用MySQL存储过程查询用户信息
		auto email = src_root["email"].get<std::string>();
		int uid = 0;            // 输出参数：用户ID
		std::string name = "";  // 输出参数：用户名
		// 调用存储过程，根据email查询对应的uid和name
		MysqlMgr::GetInstance()->TestProcedure(email, uid, name);
		cout << "email is " << email << endl;
		// 构建成功响应，包含查询到的用户信息
		root["error"] = ErrorCodes::Success;
		root["email"] = src_root["email"];
		root["name"] = name;
		root["uid"] = uid;
		std::string jsonstr = root.dump(4);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
		
	});

	// ==================== POST /get_varifycode ====================
	// 获取邮箱验证码接口
	// 处理流程：解析请求 -> 提取邮箱 -> 通过gRPC调用VarifyServer发送验证码邮件
	// 请求体JSON格式: {"email": "xxx@xxx.com"}
	// 响应体JSON格式: {"error": 0, "email": "xxx@xxx.com"}
	// 验证码由VarifyServer生成并缓存到Redis中（key为 CODEPREFIX+email）
	RegPost("/get_varifycode", [](std::shared_ptr<HttpConnection> connection) {
		// 提取请求体原始字符串
		auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive body is " << body_str << std::endl;
		connection->_response.set(http::field::content_type, "text/json");
		json root;
		// 解析请求体JSON
		auto src_root = json::parse(body_str, nullptr, false);
		if (src_root.is_discarded()) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 校验email字段是否存在
		if (!src_root.contains("email")) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 提取邮箱，通过gRPC远程调用VarifyServer获取验证码
		// VarifyServer会生成随机验证码，通过SMTP发送邮件，并将验证码存入Redis
		auto email = src_root["email"].get<std::string>();
		GetVarifyRsp rsp = VerifyGrpcClient::GetInstance()->GetVarifyCode(email);
		cout << "email is " << email << endl;
		// 将VarifyServer返回的错误码和邮箱写入响应
		root["error"] = rsp.error();
		root["email"] = src_root["email"];
		std::string jsonstr = root.dump(4);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
	});
	// ==================== POST /user_register ====================
	// 用户注册接口（day11实现）
	// 处理流程：
	//   1. 解析请求JSON，提取用户名、邮箱、密码、确认密码、头像、验证码
	//   2. 校验两次密码是否一致
	//   3. 从Redis中获取验证码并校验（防止过期和错误）
	//   4. 调用MySQL注册用户（检查用户名/邮箱是否已存在）
	//   5. 注册成功返回用户信息
	// 请求体JSON格式: {"email":"...", "user":"...", "passwd":"...", "confirm":"...", "icon":"...", "varifycode":"..."}
	RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection) {
		// 提取请求体（不打印，避免泄露密码）
		auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "text/json");
		json root;
		// 解析请求体JSON数据
		auto src_root = json::parse(body_str, nullptr, false);
		if (src_root.is_discarded()) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 从请求JSON中提取注册所需的各个字段
		auto email = src_root["email"].get<std::string>();      // 用户邮箱
		auto name = src_root["user"].get<std::string>();        // 用户名/昵称
		auto pwd = src_root["passwd"].get<std::string>();       // 密码
		auto confirm = src_root["confirm"].get<std::string>();  // 确认密码
		auto icon = src_root["icon"].get<std::string>();        // 头像（base64编码或路径）

		// 【校验1】检查两次输入的密码是否一致
		if (pwd != confirm) {
			std::cout << "password err " << std::endl;
			root["error"] = ErrorCodes::PasswdErr;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【校验2】从Redis中查找email对应的验证码是否过期
		// Redis中验证码的key格式为: CODEPREFIX + email（如 "code_xxx@xxx.com"）
		std::string  varify_code;
		bool b_get_varify = RedisMgr::GetInstance()->Get(CODEPREFIX+src_root["email"].get<std::string>(), varify_code);
		if (!b_get_varify) {
			// Redis中找不到对应key，说明验证码已过期或从未发送
			std::cout << " get varify code expired" << std::endl;
			root["error"] = ErrorCodes::VarifyExpired;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【校验3】比对用户输入的验证码与Redis中存储的验证码是否一致
		if (varify_code != src_root["varifycode"].get<std::string>()) {
			std::cout << " varify code error" << std::endl;
			root["error"] = ErrorCodes::VarifyCodeErr;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【校验4】调用MySQL注册用户，内部会检查用户名或邮箱是否已存在
		// 返回值：uid > 0 表示注册成功；uid == 0 表示用户已存在；uid == -1 表示数据库异常
		int uid = MysqlMgr::GetInstance()->RegUser(name, email, pwd, icon);
		if (uid == 0 || uid == -1) {
			std::cout << " user or email exist" << std::endl;
			root["error"] = ErrorCodes::UserExist;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}
		// 注册成功，构建响应JSON返回完整的用户信息
		root["error"] = 0;
		root["uid"] = uid;              // 新分配的用户ID
		root["email"] = email;
		root ["user"]= name;
		root["confirm"] = confirm;
		root["icon"] = icon;
		root["varifycode"] = src_root["varifycode"].get<std::string>();
		std::string jsonstr = root.dump(4);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
		});

	// ==================== POST /reset_pwd ====================
	// 重置密码接口
	// 处理流程：
	//   1. 解析请求JSON，提取用户名、邮箱、新密码、验证码
	//   2. 从Redis校验验证码（是否过期、是否正确）
	//   3. 查询MySQL验证用户名和邮箱是否匹配（防止恶意重置他人密码）
	//   4. 更新数据库中的密码
	// 请求体JSON格式: {"email":"...", "user":"...", "passwd":"...", "varifycode":"..."}
	RegPost("/reset_pwd", [](std::shared_ptr<HttpConnection> connection) {
		// 提取请求体（不打印，避免泄露密码）
		auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "text/json");
		json root;
		// 解析请求体JSON
		auto src_root = json::parse(body_str, nullptr, false);
		if (src_root.is_discarded()) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 提取重置密码所需字段
		auto email = src_root["email"].get<std::string>();  // 用户邮箱
		auto name = src_root["user"].get<std::string>();    // 用户名
		auto pwd = src_root["passwd"].get<std::string>();   // 新密码

		// 【校验1】从Redis中查找email对应的验证码是否过期
		std::string  varify_code;
		bool b_get_varify = RedisMgr::GetInstance()->Get(CODEPREFIX + src_root["email"].get<std::string>(), varify_code);
		if (!b_get_varify) {
			// 验证码已过期或不存在
			std::cout << " get varify code expired" << std::endl;
			root["error"] = ErrorCodes::VarifyExpired;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【校验2】比对用户输入的验证码是否正确
		if (varify_code != src_root["varifycode"].get<std::string>()) {
			std::cout << " varify code error" << std::endl;
			root["error"] = ErrorCodes::VarifyCodeErr;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}
		// 【校验3】查询数据库验证用户名和邮箱是否匹配
		// 防止攻击者使用自己的验证码去重置他人密码
		bool email_valid = MysqlMgr::GetInstance()->CheckEmail(name, email);
		if (!email_valid) {
			std::cout << " user email not match" << std::endl;
			root["error"] = ErrorCodes::EmailNotMatch;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【执行】所有校验通过，更新数据库中的用户密码
		bool b_up = MysqlMgr::GetInstance()->UpdatePwd(name, pwd);
		if (!b_up) {
			// 数据库更新失败（可能是连接异常等原因）
			std::cout << " update credentials failed" << std::endl;
			root["error"] = ErrorCodes::PasswdUpFailed;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 密码重置成功，返回成功响应
		std::cout << "succeed to update credentials" << std::endl;
		root["error"] = 0;
		root["email"] = email;
		root["user"] = name;
		root["varifycode"] = src_root["varifycode"].get<std::string>();
		std::string jsonstr = root.dump(4);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
		});

	// ==================== POST /user_login ====================
	// 用户登录接口
	// 处理流程：
	//   1. 解析请求JSON，提取邮箱和密码
	//   2. 查询MySQL验证邮箱和密码是否匹配，同时获取用户完整信息
	//   3. 通过普通 gRPC 调用StatusServer，为该用户分配一个可用的ChatServer
	//      并签发登录令牌 token（写入 Redis utoken_<uid>，TTL 86400s，
	//      客户端原样带给 ChatServer/ResourceServer 校验）
	//   4. 从配置文件读取ResourceServer的地址信息
	//   5. 将ChatServer连接信息、token 和ResourceServer信息返回给客户端
	// 请求体JSON格式: {"email":"...", "passwd":"..."}
	// 响应体JSON格式: {"error":0, "email":"...", "uid":123, "token":"...",
	//                  "chathost":"...", "chatport":"...", "reshost":"...", "resport":"..."}
	RegPost("/user_login", [](std::shared_ptr<HttpConnection> connection) {
		// 提取请求体（不打印 body，避免泄露密码）
		auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "text/json");
		json root;
		// 解析请求体JSON
		auto src_root = json::parse(body_str, nullptr, false);
		if (src_root.is_discarded()) {
			std::cout << "Failed to parse JSON data!" << std::endl;
			root["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 提取登录凭证
		auto email = src_root["email"].get<std::string>();  // 用户邮箱
		auto pwd = src_root["passwd"].get<std::string>();   // 用户密码
		UserInfo userInfo;  // 用于接收从数据库查询到的用户完整信息
		// 【校验1】查询MySQL验证邮箱和密码是否匹配
		bool pwd_valid = MysqlMgr::GetInstance()->CheckPwd(email, pwd, userInfo);
		if (!pwd_valid) {
			std::cout << " user credentials mismatch" << std::endl;
			root["error"] = ErrorCodes::PasswdInvalid;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【步骤2】通过普通 gRPC 调用StatusServer，为用户分配一个ChatServer
		// 返回值包含：分配的ChatServer的host、port，以及登录令牌 token
		auto reply = StatusGrpcClient::GetInstance()->GetChatServer(userInfo.uid);
		if (reply.error()) {
			// Status 返回业务错误：NoAvailableChatServer（所有 lease 缺失/过期）
			// 或其他非零（StatusServer 不可用等 RPC 失败）。均不返回空 host/port。
			int err = reply.error();
			root["error"] = (err == ErrorCodes::NoAvailableChatServer)
				? ErrorCodes::NoAvailableChatServer
				: ErrorCodes::RPCFailed;
			std::cout << " grpc get chat server failed, error is " << err << std::endl;
			std::string jsonstr = root.dump(4);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		// 【步骤3】登录成功，构建响应数据
		std::cout << "succeed to load userinfo uid is " << userInfo.uid << std::endl;
		root["error"] = 0;
		root["email"] = email;
		root["uid"] = userInfo.uid;             // 用户唯一标识ID
		root["token"] = reply.token();          // 登录令牌（utoken_<uid>，TTL 86400s）
		root["chathost"] = reply.host();        // 分配的ChatServer的IP地址
		root["chatport"] = reply.port();        // 分配的ChatServer的端口号
		// 【步骤4】从配置文件读取ResourceServer（资源服务器）的地址信息
		auto& gCfgMgr = ConfigMgr::Inst();
		std::string res_port = gCfgMgr["ResServer"]["Port"];
		std::string res_host = gCfgMgr["ResServer"]["Host"];
		root["reshost"] = res_host;          // 资源服务器IP
		root["resport"] = res_port;          // 资源服务器端口

		// 序列化JSON并写入HTTP响应体
		std::string jsonstr = root.dump(4);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
		});

	// ==================== 构建有界 worker 池（计划2.2/2.5） ====================
	// 读取 [Concurrency] 配置，缺失/非数字/<=0 时分别回退 4 / 1024
	std::size_t workers = ReadConcurrencySize("HandlerWorkers", 4);
	std::size_t capacity = ReadConcurrencySize("HandlerQueueCapacity", 1024);
	_executor = std::make_unique<HandlerExecutor>(workers, capacity);
}

/**
 * @brief 注册GET请求处理函数
 * @param url     请求的URL路径（如 "/get_test"）
 * @param handler 对应的处理函数，接收HttpConnection共享指针
 * 
 * 将url和handler的映射关系存入_get_handlers哈希表中，
 * 后续HandleGet方法根据path查表调用对应handler。
 */
void LogicSystem::RegGet(std::string url, HttpHandler handler) {
	_get_handlers.insert(make_pair(url, handler));
}

/**
 * @brief 注册POST请求处理函数
 * @param url     请求的URL路径（如 "/user_login"）
 * @param handler 对应的处理函数，接收HttpConnection共享指针
 * 
 * 将url和handler的映射关系存入_post_handlers哈希表中，
 * 后续HandlePost方法根据path查表调用对应handler。
 */
void LogicSystem::RegPost(std::string url, HttpHandler handler) {
	_post_handlers.insert(make_pair(url, handler));
}

/**
 * @brief 析构函数：回收 worker 线程
 *
 * _executor 为 unique_ptr，其析构会调用 HandlerExecutor::Stop 排空并 join，
 * 这里显式调用一次 Stop() 以便在静态析构序里更早地拒绝新投递。
 */
LogicSystem::~LogicSystem() {
	Stop();
}

/**
 * @brief 幂等停止：拒绝新任务、排空既有 handler、回收 worker 线程
 *
 * 供 GateServer 关停信号回调在 AsioIOServicePool::Stop()/ioc.stop() 前调用。
 * 排空期间 worker 仍可向连接的 socket executor post-back 完成响应。
 */
void LogicSystem::Stop() {
	if (_executor) {
		_executor->Stop();
	}
}

/**
 * @brief 唯一请求入口：查路由表并把命中的 handler 投递到有界 worker 池（计划2.2）
 *
 * 路由查找使用构造后只读的 _get_handlers/_post_handlers；命中后只把现有
 * handler 投递到执行器。worker 上继续使用当前同步 mysql-concpp、hiredis
 * 和同步 gRPC 客户端。handler 完成或抛异常后必须
 * boost::asio::post(connection->GetExecutor(), ...) 回到 _socket.get_executor()，
 * 再设置最终状态并调用 WriteResponse()；worker 线程不得直接触碰 socket/timer。
 *
 * _request/_response 修改保持“读完成 → 单个 worker → executor 完成”
 * 这条单所有者链：读在 socket executor 完成、handler 在 worker 上写 body、
 * post-back 回到 socket executor 设置状态并发送，三者不重叠。
 */
LogicSystem::DispatchResult LogicSystem::Dispatch(http::verb method, std::string path,
	std::shared_ptr<HttpConnection> connection) {
	// 选择构造后只读的路由表
	const std::map<std::string, HttpHandler>* table = nullptr;
	if (method == http::verb::get) {
		table = &_get_handlers;
	}
	else if (method == http::verb::post) {
		table = &_post_handlers;
	}
	else {
		return DispatchResult::NotFound;
	}

	// 路由查找
	auto it = table->find(path);
	if (it == table->end()) {
		return DispatchResult::NotFound;
	}

	// 停机中：不再接收新 handler
	if (_executor->IsStopping()) {
		return DispatchResult::Stopping;
	}

	// 拷贝一份 handler 闭包与连接指针，避免引用 map 内节点及裸指针
	HttpHandler handler = it->second;
	auto con = connection;

	// 把现有 handler 投递到执行器；worker 完成或抛异常后 post-back 回 socket executor
	bool ok = _executor->Post([con, handler]() {
		try {
			handler(con);
		}
		catch (...) {
			// handler 异常：回到 socket executor 写 HTTP 500 + 同一 JSON
			boost::asio::post(con->GetExecutor(), [con]() {
				// deadline 可能已关闭 socket，此时只结束生命周期，不再写响应
				if (!con->GetSocket().is_open()) {
					return;
				}
				con->_response.result(http::status::internal_server_error);
				con->_response.set(http::field::content_type, "text/json");
				boost::beast::ostream(con->_response.body()) << R"({"error":1002})";
				con->WriteResponse();
			});
			return;
		}
		// 成功：回到 socket executor 设置最终状态并写响应（200 + Server 头）
		boost::asio::post(con->GetExecutor(), [con]() {
			// deadline 可能已关闭 socket，此时只结束生命周期，不再写响应
			if (!con->GetSocket().is_open()) {
				return;
			}
			con->_response.result(http::status::ok);
			con->_response.set(http::field::server, "GateServer");
			con->WriteResponse();
		});
	});

	if (!ok) {
		// Post 失败：再次确认是否因停机（区分 Stopping 与 Overloaded）
		return _executor->IsStopping() ? DispatchResult::Stopping : DispatchResult::Overloaded;
	}
	return DispatchResult::Accepted;
}