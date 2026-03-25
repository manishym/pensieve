#pragma once

#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pensieve {

// Request collapsing for thundering-herd protection.  When N coroutines
// request the same key concurrently, only the first (the "initiator")
// performs the actual fetch.  The remaining N-1 suspend and are resumed
// with the shared result once the initiator calls complete().
class WaitGroup {
public:
    struct PendingFetch {
        std::vector<std::coroutine_handle<>> waiters;
        std::optional<std::string> result;
        bool completed = false;
    };

    // Returns true if the caller is the initiator (first request for this key).
    // Returns false if another fetch is already in-flight — the caller must
    // then co_await wait(key) to get the result.
    bool try_join(const std::string& key);

    // Awaitable that suspends the caller until the initiator calls complete().
    class WaitAwaitable {
    public:
        WaitAwaitable(WaitGroup& wg, const std::string& key)
            : wg_(wg), key_(key) {}

        bool await_ready() const noexcept { return false; }

        // Returns true to suspend, false to continue immediately.
        bool await_suspend(std::coroutine_handle<> h);

        std::optional<std::string> await_resume();

    private:
        WaitGroup& wg_;
        std::string key_;
        std::shared_ptr<PendingFetch> fetch_;
    };

    WaitAwaitable wait(const std::string& key);

    // Called by the initiator when the fetch completes.  Resumes all waiting
    // coroutines with the result.
    void complete(const std::string& key, std::optional<std::string> value);

    size_t pending_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<PendingFetch>> pending_;
};

}  // namespace pensieve
