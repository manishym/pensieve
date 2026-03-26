#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sdk/pensieve_client.h"

using Clock = std::chrono::steady_clock;
using Micros = std::chrono::microseconds;

struct BenchConfig {
    std::string seed_host;
    uint16_t seed_port = 11211;
    size_t num_keys = 10000;
    size_t value_size = 128;
    int wait_secs = 0;
    int threads = 1;
    std::string output_file;
};

struct LatencyReport {
    std::string label;
    size_t total_ops = 0;
    size_t errors = 0;
    double elapsed_secs = 0.0;
    std::vector<int64_t> latencies_us;
};

static const int64_t kBucketCeils[] = {
    50, 100, 250, 500, 1000, 5000, 10000, 50000, 100000
};
static constexpr size_t kNumBuckets = 10;

static const char* kBucketLabels[] = {
    "  < 50us ", "  < 100us", "  < 250us", "  < 500us",
    "  <   1ms", "  <   5ms", "  <  10ms", "  <  50ms",
    "  < 100ms", "  >= 100ms"
};

static bool parse_seed(const std::string& s, std::string& host,
                       uint16_t& port) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) { host = s; port = 11211; return true; }
    host = s.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(s.substr(colon + 1)));
    return true;
}

static std::string random_hex(std::mt19937& rng, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out(len, '\0');
    std::uniform_int_distribution<int> dist(0, 15);
    for (size_t i = 0; i < len; ++i) out[i] = hex[dist(rng)];
    return out;
}

static int64_t percentile(const std::vector<int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(
        p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

static void print_histogram(const std::vector<int64_t>& latencies) {
    size_t buckets[kNumBuckets] = {};
    for (int64_t us : latencies) {
        size_t b = kNumBuckets - 1;
        for (size_t i = 0; i < kNumBuckets - 1; ++i) {
            if (us < kBucketCeils[i]) { b = i; break; }
        }
        ++buckets[b];
    }

    size_t max_count = *std::max_element(buckets, buckets + kNumBuckets);
    constexpr size_t bar_width = 40;

    std::cout << "\n  Latency distribution:\n";
    for (size_t i = 0; i < kNumBuckets; ++i) {
        size_t bw = max_count > 0
            ? (buckets[i] * bar_width / max_count) : 0;
        double pct = latencies.empty() ? 0.0
            : 100.0 * static_cast<double>(buckets[i])
                     / static_cast<double>(latencies.size());
        std::cout << "    " << kBucketLabels[i] << " |";
        for (size_t j = 0; j < bw; ++j) std::cout << '#';
        std::cout << " " << buckets[i]
                  << " (" << std::fixed << std::setprecision(1)
                  << pct << "%)\n";
    }
}

static void print_report(LatencyReport& report) {
    std::sort(report.latencies_us.begin(), report.latencies_us.end());

    double throughput = report.elapsed_secs > 0
        ? static_cast<double>(report.total_ops) / report.elapsed_secs : 0;

    std::cout << "\n===== " << report.label << " =====\n"
              << "  Total ops:    " << report.total_ops << "\n"
              << "  Errors:       " << report.errors << "\n"
              << "  Elapsed:      " << std::fixed << std::setprecision(3)
              << report.elapsed_secs << " s\n"
              << "  Throughput:   " << std::fixed << std::setprecision(0)
              << throughput << " ops/sec\n";

    if (!report.latencies_us.empty()) {
        std::cout << "  p50:          "
                  << percentile(report.latencies_us, 0.50) << " us\n"
                  << "  p95:          "
                  << percentile(report.latencies_us, 0.95) << " us\n"
                  << "  p99:          "
                  << percentile(report.latencies_us, 0.99) << " us\n"
                  << "  min:          "
                  << report.latencies_us.front() << " us\n"
                  << "  max:          "
                  << report.latencies_us.back() << " us\n";
        print_histogram(report.latencies_us);
    }
    std::cout << std::flush;
}

static void compute_buckets(const std::vector<int64_t>& latencies,
                            size_t out[kNumBuckets]) {
    for (size_t i = 0; i < kNumBuckets; ++i) out[i] = 0;
    for (int64_t us : latencies) {
        size_t b = kNumBuckets - 1;
        for (size_t i = 0; i < kNumBuckets - 1; ++i) {
            if (us < kBucketCeils[i]) { b = i; break; }
        }
        ++out[b];
    }
}

static std::string report_to_json(LatencyReport& report) {
    std::sort(report.latencies_us.begin(), report.latencies_us.end());
    double throughput = report.elapsed_secs > 0
        ? static_cast<double>(report.total_ops) / report.elapsed_secs : 0;

    size_t buckets[kNumBuckets] = {};
    compute_buckets(report.latencies_us, buckets);

    std::ostringstream js;
    js << "{"
       << "\"total_ops\":" << report.total_ops
       << ",\"errors\":" << report.errors
       << ",\"elapsed_secs\":" << std::fixed << std::setprecision(6)
       << report.elapsed_secs
       << ",\"throughput\":" << std::setprecision(1) << throughput
       << ",\"p50\":" << percentile(report.latencies_us, 0.50)
       << ",\"p95\":" << percentile(report.latencies_us, 0.95)
       << ",\"p99\":" << percentile(report.latencies_us, 0.99)
       << ",\"min\":" << (report.latencies_us.empty()
                          ? 0 : report.latencies_us.front())
       << ",\"max\":" << (report.latencies_us.empty()
                          ? 0 : report.latencies_us.back())
       << ",\"buckets\":[";
    for (size_t i = 0; i < kNumBuckets; ++i) {
        if (i > 0) js << ",";
        js << buckets[i];
    }
    js << "]}";
    return js.str();
}

static void write_json_output(const std::string& path,
                               LatencyReport& put_report,
                               LatencyReport& get_report,
                               const BenchConfig& cfg) {
    std::ofstream f(path);
    f << "{\"threads\":" << cfg.threads
      << ",\"num_keys\":" << cfg.num_keys
      << ",\"value_size\":" << cfg.value_size
      << ",\"put\":" << report_to_json(put_report)
      << ",\"get\":" << report_to_json(get_report)
      << "}\n";
    f.close();
    std::cout << "Results written to " << path << "\n" << std::flush;
}

static void print_sdk_stats(pensieve::PensieveClient& client) {
    auto s = client.stats();
    std::cout << "\n===== SDK Stats =====\n"
              << "  requests_total:   " << s.requests_total << "\n"
              << "  requests_direct:  " << s.requests_direct << "\n"
              << "  requests_retried: " << s.requests_retried << "\n"
              << "  ring_refreshes:   " << s.ring_refreshes << "\n"
              << "  circuit_breaks:   " << s.circuit_breaks << "\n"
              << std::flush;
}

struct ThreadResult {
    std::vector<int64_t> latencies;
    size_t errors = 0;
};

static void worker_put(pensieve::PensieveClient& client,
                       const std::vector<std::string>& keys,
                       const std::string& value,
                       size_t begin, size_t end,
                       ThreadResult& out) {
    out.latencies.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        auto t0 = Clock::now();
        bool ok = client.put(keys[i], value);
        auto t1 = Clock::now();
        out.latencies.push_back(
            std::chrono::duration_cast<Micros>(t1 - t0).count());
        if (!ok) ++out.errors;
    }
}

static void worker_get(pensieve::PensieveClient& client,
                       const std::vector<std::string>& keys,
                       size_t begin, size_t end,
                       ThreadResult& out) {
    out.latencies.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        auto t0 = Clock::now();
        auto val = client.get(keys[i]);
        auto t1 = Clock::now();
        out.latencies.push_back(
            std::chrono::duration_cast<Micros>(t1 - t0).count());
        if (!val.has_value()) ++out.errors;
    }
}

using WorkerClients = std::vector<std::unique_ptr<pensieve::PensieveClient>>;

static WorkerClients make_clients(const BenchConfig& cfg, int n) {
    WorkerClients clients;
    clients.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        pensieve::PensieveClient::Config sdk_cfg;
        sdk_cfg.seed_host = cfg.seed_host;
        sdk_cfg.seed_port = cfg.seed_port;
        sdk_cfg.max_connections_per_node = 4;
        sdk_cfg.refresh_interval = std::chrono::seconds{30};
        auto c = std::make_unique<pensieve::PensieveClient>(sdk_cfg);
        if (!c->connect()) return {};
        clients.push_back(std::move(c));
    }
    return clients;
}

static LatencyReport run_threaded_puts(WorkerClients& clients,
                                       const std::vector<std::string>& keys,
                                       const std::string& value) {
    size_t n = clients.size();
    size_t per = keys.size() / n;
    std::vector<ThreadResult> results(n);
    std::vector<std::thread> threads;

    auto wall_start = Clock::now();
    for (size_t t = 0; t < n; ++t) {
        size_t begin = t * per;
        size_t end = (t + 1 == n) ? keys.size() : begin + per;
        threads.emplace_back(worker_put, std::ref(*clients[t]),
                             std::cref(keys), std::cref(value),
                             begin, end, std::ref(results[t]));
    }
    for (auto& th : threads) th.join();
    auto wall_end = Clock::now();

    LatencyReport report;
    report.label = "PUT";
    report.elapsed_secs = std::chrono::duration<double>(
        wall_end - wall_start).count();
    for (auto& r : results) {
        report.errors += r.errors;
        report.latencies_us.insert(report.latencies_us.end(),
                                   r.latencies.begin(), r.latencies.end());
    }
    report.total_ops = report.latencies_us.size();
    return report;
}

static LatencyReport run_threaded_gets(WorkerClients& clients,
                                       const std::vector<std::string>& keys) {
    size_t n = clients.size();
    size_t per = keys.size() / n;
    std::vector<ThreadResult> results(n);
    std::vector<std::thread> threads;

    auto wall_start = Clock::now();
    for (size_t t = 0; t < n; ++t) {
        size_t begin = t * per;
        size_t end = (t + 1 == n) ? keys.size() : begin + per;
        threads.emplace_back(worker_get, std::ref(*clients[t]),
                             std::cref(keys),
                             begin, end, std::ref(results[t]));
    }
    for (auto& th : threads) th.join();
    auto wall_end = Clock::now();

    LatencyReport report;
    report.label = "GET";
    report.elapsed_secs = std::chrono::duration<double>(
        wall_end - wall_start).count();
    for (auto& r : results) {
        report.errors += r.errors;
        report.latencies_us.insert(report.latencies_us.end(),
                                   r.latencies.begin(), r.latencies.end());
    }
    report.total_ops = report.latencies_us.size();
    return report;
}

static BenchConfig parse_args(int argc, const char* const argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc)
            parse_seed(argv[++i], cfg.seed_host, cfg.seed_port);
        else if (arg == "--num-keys" && i + 1 < argc)
            cfg.num_keys = static_cast<size_t>(std::stoul(argv[++i]));
        else if (arg == "--value-size" && i + 1 < argc)
            cfg.value_size = static_cast<size_t>(std::stoul(argv[++i]));
        else if (arg == "--wait" && i + 1 < argc)
            cfg.wait_secs = std::stoi(argv[++i]);
        else if (arg == "--threads" && i + 1 < argc)
            cfg.threads = std::stoi(argv[++i]);
        else if (arg == "--output" && i + 1 < argc)
            cfg.output_file = argv[++i];
    }
    return cfg;
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --seed HOST:PORT [--num-keys N] [--value-size B]"
              << " [--wait SECS] [--threads T] [--output FILE]\n";
}

static bool connect_with_retries(pensieve::PensieveClient& client,
                                 int max_retries) {
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (client.connect()) return true;
        std::cerr << "  connect attempt " << (attempt + 1)
                  << " failed, retrying in 2s...\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds{2});
    }
    return false;
}

int main(int argc, char* argv[]) {
    auto bench_cfg = parse_args(argc, argv);
    if (bench_cfg.seed_host.empty()) {
        usage(argv[0]);
        return 1;
    }

    if (bench_cfg.wait_secs > 0) {
        std::cout << "Waiting " << bench_cfg.wait_secs
                  << "s for cluster convergence...\n" << std::flush;
        std::this_thread::sleep_for(
            std::chrono::seconds(bench_cfg.wait_secs));
    }

    std::cout << "Pensieve Benchmark\n"
              << "  seed:       " << bench_cfg.seed_host
              << ":" << bench_cfg.seed_port << "\n"
              << "  num_keys:   " << bench_cfg.num_keys << "\n"
              << "  value_size: " << bench_cfg.value_size << " bytes\n"
              << "  threads:    " << bench_cfg.threads << "\n\n"
              << std::flush;

    pensieve::PensieveClient::Config sdk_cfg;
    sdk_cfg.seed_host = bench_cfg.seed_host;
    sdk_cfg.seed_port = bench_cfg.seed_port;
    sdk_cfg.max_connections_per_node = 4;
    sdk_cfg.refresh_interval = std::chrono::seconds{30};

    pensieve::PensieveClient client(sdk_cfg);
    if (!connect_with_retries(client, 5)) {
        std::cerr << "error: could not connect to seed after retries\n";
        return 1;
    }

    std::cout << client.cluster_info() << std::flush;

    std::mt19937 rng(42);
    std::vector<std::string> keys;
    keys.reserve(bench_cfg.num_keys);
    for (size_t i = 0; i < bench_cfg.num_keys; ++i)
        keys.push_back("bench:" + random_hex(rng, 16));

    std::string value = random_hex(rng, bench_cfg.value_size);

    auto worker_clients = make_clients(bench_cfg, bench_cfg.threads);
    if (worker_clients.empty()) {
        std::cerr << "error: could not connect worker clients\n";
        return 1;
    }

    std::cout << "--- Phase 1: PUT " << keys.size() << " keys ("
              << bench_cfg.threads << " threads) ---\n" << std::flush;
    auto put_report = run_threaded_puts(worker_clients, keys, value);
    print_report(put_report);

    std::cout << "\n--- Phase 2: GET " << keys.size() << " keys ("
              << bench_cfg.threads << " threads) ---\n" << std::flush;
    auto get_report = run_threaded_gets(worker_clients, keys);
    print_report(get_report);

    print_sdk_stats(client);

    if (!bench_cfg.output_file.empty())
        write_json_output(bench_cfg.output_file, put_report, get_report,
                          bench_cfg);

    std::cout << "\nDone.\n" << std::flush;
    return 0;
}
