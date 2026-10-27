// PC simulator entry point. The loop below mirrors Arduino's
// setup()/loop(): construct the platform, App::begin(), then tick().
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../core/app.h"
#include "../core/util.h"
#include "../core/web_api.h"
#include "pc_platform.h"
#include "sim_bms.h"

using namespace bms;

static volatile sig_atomic_t gStop = 0;

static void usage() {
    printf("usage: bms_agent_sim [--http-port 8080] [--snmp-port N] [--data-dir ./data]\n"
           "                     [--web-dir ./web] [--rollback-sec 120]\n");
}

int main(int argc, char** argv) {
    uint16_t httpPort = 8080;
    std::string dataDir = "./data", webDir = "./web";
    AppOptions opts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--http-port" && v) httpPort = (uint16_t)atoi(argv[++i]);
        else if (a == "--snmp-port" && v) opts.snmpPortOverride = (uint16_t)atoi(argv[++i]);
        else if (a == "--data-dir" && v) dataDir = argv[++i];
        else if (a == "--web-dir" && v) webDir = argv[++i];
        else if (a == "--rollback-sec" && v) opts.networkRollbackS = (uint32_t)atoi(argv[++i]);
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    signal(SIGINT, [](int) { gStop = 1; });
    signal(SIGTERM, [](int) { gStop = 1; });

    PcSystem sys;
    FileStorage storage(dataDir);
    SimNetwork net;
    SimBms sim(sys);
    PosixUdp snmpUdp, ntpUdp;
    Platform platform{storage, net, sim, snmpUdp, ntpUdp, sys};

    HttpServer http;
    if (!http.begin(httpPort)) return 1;

    while (!gStop) {
        // One iteration of this loop = one "boot" of the device.
        App app(platform, opts);
        WebApi api(app);
        api.setExtension([&sim](const HttpRequest& req, HttpResponse& res) {
            if (req.path != "/api/sim") return false;
            JsonDocument doc;
            if (req.method == "PUT") {
                JsonDocument body;
                if (deserializeJson(body, req.body)) {
                    jsonError(res, 400, "invalid JSON body");
                    return true;
                }
                std::string err = sim.fromJson(body.as<JsonVariantConst>());
                if (!err.empty()) {
                    jsonError(res, 400, err);
                    return true;
                }
            } else if (req.method != "GET") {
                jsonError(res, 405, "method not allowed");
                return true;
            }
            sim.toJson(doc.to<JsonObject>());
            serializeJson(doc, res.body);
            return true;
        });
        app.begin();

        auto handler = [&](const HttpRequest& req, HttpResponse& res) {
            if (api.handle(req, res)) return;
            if (req.method != "GET" || !serveStatic(webDir, req.path, res)) jsonError(res, 404, "not found");
        };

        while (!gStop && !sys.rebootRequested) {
            app.tick();
            http.poll(handler);
            if (api.rebootRequested()) sys.reboot();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (sys.rebootRequested) {
            logf("---- simulated reboot ----");
            sys.rebootRequested = false;
        }
        snmpUdp.stop();
        ntpUdp.stop();
    }
    logf("bye");
    return 0;
}
