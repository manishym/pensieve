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
        std::mutex mu;
        std::vector<std::coroutine_handle<>> waiters;
        std::optional<std::string> result;
        bool completed = false;
    };

    using FetchHandle = std::shared_ptr<PendingFetch>;

    struct JoinResult {
        bool is_initiator;
        FetchHandle handle;
    };

    // Atomically join the wait group for a key.  Returns a handle that the
    // caller must hold.  If is_initiator is true, the caller performs the
    // actual fetch and must call complete().  Otherwise, the caller passes
    // the handle to co_await wait(handle).
    JoinResult try_join(const std::string& key);

    // Awaitable that suspends until the initiator calls complete().
    // Operates directly on the FetchHandle -- no map lookup required,
    // eliminating the TOCTOU race between try_join and wait.
    class WaitAwaitable {
    public:
        explicit WaitAwaitable(FetchHandle fetch)
            : fetch_(std::move(fetch)) {}

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> h);
        std::optional<std::string> await_resume();

    private:
        FetchHandle fetch_;
    };

    WaitAwaitable wait(FetchHandle fetch);

    // Called by the initiator when the fetch completes.  Resumes all waiting
    // coroutines with the result.
    void complete(const std::string& key, std::optional<std::string> value);

    size_t pending_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, FetchHandle> pending_;
};

}  // namespace pensieve
