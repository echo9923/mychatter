#pragma once
#include "Singleton.h"
#include <functional>
#include <map>
#include <memory>
#include "const.h"

class HttpConnection;
class HandlerExecutor;
typedef std::function<void(std::shared_ptr<HttpConnection>)> HttpHandler;
class LogicSystem :public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	/**
	 * @brief 请求分发结果（计划2.2）
	 *
	 * Dispatch 的唯一返回类型，精确区分四种结果，由 HttpConnection::HandleReq
	 * 据此决定立即写响应还是等待 worker post-back。
	 */
	enum class DispatchResult {
		Accepted,    ///< 已命中路由并成功投递到 worker 池，响应由 post-back 完成
		NotFound,    ///< 未匹配到路由（HTTP 404）
		Overloaded,  ///< worker 池"运行中+排队"已满，拒绝投递（HTTP 503）
		Stopping,    ///< worker 池已停止接收（HTTP 503）
	};

	~LogicSystem();

	/**
	 * @brief 唯一请求入口（计划2.2）
	 *
	 * 在构造后只读的 _get_handlers/_post_handlers 中查找路由；命中后把现有
	 * handler 投递到有界 worker 池执行，worker 完成或抛异常后通过
	 * boost::asio::post(connection->GetExecutor(), ...) 回到连接的 socket
	 * executor 再设置最终状态并 WriteResponse，worker 线程绝不直接触碰
	 * socket/timer。
	 * @param method     HTTP 方法（get/post，其它视作 NotFound）
	 * @param path       请求路径（GET 为去掉查询串的 _get_url，POST 为 target）
	 * @param connection HTTP 连接共享指针
	 * @return 见 DispatchResult
	 */
	DispatchResult Dispatch(http::verb method, std::string path, std::shared_ptr<HttpConnection> connection);

	/**
	 * @brief 幂等停止：拒绝新任务、排空既有 handler、回收 worker 线程
	 *
	 * 供 GateServer 关停信号回调在 ioc.stop() 前调用，确保在途 handler
	 * 经 post-back 完成响应。多次调用安全。
	 */
	void Stop();

	void RegGet(std::string, HttpHandler handler);
	void RegPost(std::string, HttpHandler handler);
private:
	LogicSystem();
	std::map<std::string, HttpHandler> _post_handlers;
	std::map<std::string, HttpHandler> _get_handlers;
	/// 有界 handler worker 池（计划2.1/2.2），构造时按 config 建池
	std::unique_ptr<HandlerExecutor> _executor;
};
