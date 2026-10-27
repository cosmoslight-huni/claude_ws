// REST API for the Web UI. Transport-agnostic: the platform's HTTP
// server turns a request into HttpRequest and sends back HttpResponse.
#pragma once

#include <functional>
#include <map>
#include <string>

#include "app.h"

namespace bms {

struct HttpRequest {
    std::string method;
    std::string path;          // without query string
    std::string authorization; // raw "Authorization" header
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

class WebApi {
public:
    // Platform specific routes (e.g. /api/sim on the PC). Called for
    // authenticated requests the core does not know; return false if unhandled.
    using Extension = std::function<bool(const HttpRequest&, HttpResponse&)>;

    explicit WebApi(App& app) : app_(app) {}
    void setExtension(Extension e) { ext_ = std::move(e); }

    // Returns false when the path is not an API path (serve a static file).
    bool handle(const HttpRequest& req, HttpResponse& res);

    // Set when a handled request asked for a reboot; the platform reboots
    // after the response has been sent.
    bool rebootRequested() const { return reboot_; }

private:
    bool authorized(const HttpRequest& req);
    void login(const HttpRequest& req, HttpResponse& res);
    void route(const HttpRequest& req, HttpResponse& res);

    App& app_;
    Extension ext_;
    std::map<std::string, uint64_t> sessions_;  // token -> expiry (millis)
    uint32_t failedLogins_ = 0;
    uint64_t lockedUntil_ = 0;
    bool reboot_ = false;
};

void jsonError(HttpResponse& res, int status, const std::string& msg);

}  // namespace bms
