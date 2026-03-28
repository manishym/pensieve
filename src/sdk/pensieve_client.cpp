#include "sdk/pensieve_client.h"

#include <chrono>

#include "protocol/message.h"
#include "sdk/circuit_breaker.h"
#include "sdk/connection_pool.h"
#include "sdk/topology_manager.h"

namespace pensieve {

struct PensieveClient::Impl {
    Config cfg;
    TopologyManager topo;
    ConnectionPool pool;
    CircuitBreaker breaker;
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> requests_direct{0};
    std::atomic<uint64_t> requests_retried{0};
    std::atomic<uint64_t> ring_refreshes{0};
    std::atomic<uint64_t> circuit_breaks{0};
    std::atomic<uint64_t> last_latency_us{0};

    explicit Impl(Config c)
        : cfg(std::move(c)),
          pool(cfg.max_connections_per_node),
          breaker({cfg.circuit_break_threshold,
                   cfg.circuit_break_duration}) {}

    void on_failure(const NodeEndpoint& ep, bool& first_attempt) {
        breaker.record_failure(ep);
        if (first_attempt) ++requests_retried;
        first_attempt = false;
    }

    std::optional<Response> execute(Opcode op, std::string_view key,
                                    std::string_view value) {
        auto start = std::chrono::steady_clock::now();
        ++requests_total;

        auto targets = topo.find_successors(key,
            static_cast<size_t>(cfg.max_retries) + 1);
        if (targets.empty()) return std::nullopt;

        bool first_attempt = true;
        for (const auto& ep : targets) {
            if (!breaker.allow(ep)) {
                ++circuit_breaks;
                continue;
            }

            int fd = pool.acquire(ep);
            if (fd < 0) {
                on_failure(ep, first_attempt);
                continue;
            }

            Request req;
            req.opcode = op;
            req.key = key;
            req.value = value;

            if (!ConnectionPool::send_request(fd, req)) {
                pool.discard(fd);
                on_failure(ep, first_attempt);
                continue;
            }

            auto resp = ConnectionPool::recv_response(fd);
            if (!resp.has_value()) {
                pool.discard(fd);
                on_failure(ep, first_attempt);
                continue;
            }

            pool.release(ep, fd);
            breaker.record_success(ep);

            if (first_attempt) ++requests_direct;

            auto elapsed = std::chrono::steady_clock::now() - start;
            last_latency_us.store(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        elapsed).count()));

            if (resp->status == Status::Error && resp->value.empty()) {
                topo.refresh();
                ++ring_refreshes;
            }

            return resp;
        }
        return std::nullopt;
    }
};

PensieveClient::PensieveClient(Config cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

PensieveClient::~PensieveClient() = default;

bool PensieveClient::connect() {
    if (!impl_->topo.bootstrap(impl_->cfg.seed_host, impl_->cfg.seed_port))
        return false;
    impl_->topo.start_background_refresh(impl_->cfg.refresh_interval);
    return true;
}

std::optional<std::string> PensieveClient::get(std::string_view key) {
    auto resp = impl_->execute(Opcode::Get, key, {});
    if (!resp.has_value()) return std::nullopt;
    if (resp->status == Status::Ok) return std::move(resp->value);
    return std::nullopt;
}

bool PensieveClient::put(std::string_view key, std::string_view value) {
    auto resp = impl_->execute(Opcode::Set, key, value);
    return resp.has_value() && resp->status == Status::Ok;
}

bool PensieveClient::del(std::string_view key) {
    auto resp = impl_->execute(Opcode::Del, key, {});
    return resp.has_value() && resp->status == Status::Ok;
}

std::string PensieveClient::cluster_info() {
    auto endpoints = impl_->topo.all_endpoints();
    std::string out;
    out += "Cluster: " + std::to_string(endpoints.size()) + " node(s)\n";
    for (const auto& ep : endpoints) {
        out += "  " + ep.host + ":" + std::to_string(ep.data_port) +
               " (gossip " + std::to_string(ep.gossip_port) + ")\n";
    }
    return out;
}

PensieveClient::Stats PensieveClient::stats() const {
    return {
        impl_->requests_total.load(),
        impl_->requests_direct.load(),
        impl_->requests_retried.load(),
        impl_->ring_refreshes.load(),
        impl_->circuit_breaks.load(),
        impl_->last_latency_us.load(),
    };
}

}  // namespace pensieve
