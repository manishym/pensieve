#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

#include "sdk/pensieve_client.h"

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --seed HOST:PORT <command> [args...]\n"
              << "\nCommands:\n"
              << "  get KEY            Retrieve value for KEY\n"
              << "  put KEY VALUE      Store VALUE under KEY\n"
              << "  del KEY            Delete KEY\n"
              << "  info               Print cluster topology\n"
              << "  stats              Print client statistics\n";
}

static bool parse_seed(const std::string& seed, std::string& host,
                       uint16_t& port) {
    auto colon = seed.rfind(':');
    if (colon == std::string::npos) {
        host = seed;
        port = 11211;
        return true;
    }
    host = seed.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(seed.substr(colon + 1)));
    return true;
}

struct CmdContext {
    pensieve::PensieveClient& client;
    int argc;
    char** argv;
    int cmd_start;
};

static int cmd_get(CmdContext& ctx) {
    if (ctx.cmd_start + 1 >= ctx.argc) {
        std::cerr << "error: get requires KEY\n";
        return 1;
    }
    auto val = ctx.client.get(ctx.argv[ctx.cmd_start + 1]);
    if (val.has_value()) {
        std::cout << "OK: " << *val << "\n";
        return 0;
    }
    std::cout << "NOT_FOUND\n";
    return 1;
}

static int cmd_put(CmdContext& ctx) {
    if (ctx.cmd_start + 2 >= ctx.argc) {
        std::cerr << "error: put requires KEY VALUE\n";
        return 1;
    }
    bool ok = ctx.client.put(ctx.argv[ctx.cmd_start + 1],
                             ctx.argv[ctx.cmd_start + 2]);
    std::cout << (ok ? "OK" : "ERROR") << "\n";
    return ok ? 0 : 1;
}

static int cmd_del(CmdContext& ctx) {
    if (ctx.cmd_start + 1 >= ctx.argc) {
        std::cerr << "error: del requires KEY\n";
        return 1;
    }
    bool ok = ctx.client.del(ctx.argv[ctx.cmd_start + 1]);
    std::cout << (ok ? "OK" : "NOT_FOUND") << "\n";
    return ok ? 0 : 1;
}

static int cmd_info(CmdContext& ctx) {
    std::cout << ctx.client.cluster_info();
    return 0;
}

static int cmd_stats(CmdContext& ctx) {
    auto s = ctx.client.stats();
    std::cout << "requests_total:   " << s.requests_total << "\n"
              << "requests_direct:  " << s.requests_direct << "\n"
              << "requests_retried: " << s.requests_retried << "\n"
              << "ring_refreshes:   " << s.ring_refreshes << "\n"
              << "circuit_breaks:   " << s.circuit_breaks << "\n"
              << "last_latency_us:  " << s.last_latency_us << "\n";
    return 0;
}

using CmdHandler = std::function<int(CmdContext&)>;

static const std::unordered_map<std::string, CmdHandler>& commands() {
    static const std::unordered_map<std::string, CmdHandler> tbl = {
        {"get", cmd_get}, {"put", cmd_put}, {"del", cmd_del},
        {"info", cmd_info}, {"stats", cmd_stats},
    };
    return tbl;
}

int main(int argc, char* argv[]) {
    if (argc < 4) { usage(argv[0]); return 1; }

    std::string seed_str;
    int cmd_start = 1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            seed_str = argv[++i];
            cmd_start = i + 1;
        }
    }

    if (seed_str.empty()) {
        std::cerr << "error: --seed is required\n";
        usage(argv[0]);
        return 1;
    }
    if (cmd_start >= argc) { usage(argv[0]); return 1; }

    pensieve::PensieveClient::Config cfg;
    if (!parse_seed(seed_str, cfg.seed_host, cfg.seed_port)) {
        std::cerr << "error: invalid seed format\n";
        return 1;
    }
    cfg.refresh_interval = std::chrono::seconds{0};

    pensieve::PensieveClient client(cfg);
    if (!client.connect()) {
        std::cerr << "error: could not connect to seed "
                  << seed_str << "\n";
        return 1;
    }

    auto it = commands().find(argv[cmd_start]);
    if (it == commands().end()) {
        std::cerr << "error: unknown command '" << argv[cmd_start] << "'\n";
        usage(argv[0]);
        return 1;
    }

    CmdContext ctx{client, argc, argv, cmd_start};
    return it->second(ctx);
}
