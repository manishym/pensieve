#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace pensieve {

template <typename T = void>
class Task;

namespace detail {

struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> finishing) noexcept {
        if (auto caller = finishing.promise().caller) {
            return caller;
        }
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

template <typename T>
struct TaskPromise {
    std::coroutine_handle<> caller = nullptr;
    std::optional<T> value;
    std::exception_ptr exception;

    Task<T> get_return_object();

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_value(T v) { value.emplace(std::move(v)); }

    void unhandled_exception() { exception = std::current_exception(); }

    T result() {
        if (exception) std::rethrow_exception(exception);
        return std::move(*value);
    }
};

template <>
struct TaskPromise<void> {
    std::coroutine_handle<> caller = nullptr;
    std::exception_ptr exception;

    Task<void> get_return_object();

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() {}

    void unhandled_exception() { exception = std::current_exception(); }

    void result() {
        if (exception) std::rethrow_exception(exception);
    }
};

}  // namespace detail

template <typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;

    Task() = default;

    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    ~Task() {
        if (handle_) handle_.destroy();
    }

    Task(Task&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
        handle_.promise().caller = caller;
        return handle_;
    }

    T await_resume() { return handle_.promise().result(); }

    void start() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    bool done() const { return !handle_ || handle_.done(); }

    std::coroutine_handle<promise_type> handle() const { return handle_; }

private:
    std::coroutine_handle<promise_type> handle_ = nullptr;
};

namespace detail {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
    return Task<T>{
        std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() {
    return Task<void>{
        std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

}  // namespace detail

}  // namespace pensieve
