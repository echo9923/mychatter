// im_http_client.h — minimal Boost.Beast HTTP/1.1 client for the gate-smoke
// scenario (plan Verification.6: gate-smoke). Synchronous, with a per-stream
// timeout so a wedged handler cannot hang the test.
#pragma once

#include <string>

namespace imt {

struct HttpResponse {
	int         status  = 0;     // HTTP status code (0 on transport failure)
	std::string body;            // response body
	std::string error;           // empty unless status==0 (transport error)
};

HttpResponse HttpGet (const std::string& host, unsigned short port,
                      const std::string& target, int timeout_ms);
HttpResponse HttpPost(const std::string& host, unsigned short port,
                      const std::string& target, const std::string& body,
                      const std::string& content_type, int timeout_ms);

} // namespace imt
