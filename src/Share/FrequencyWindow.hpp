#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Sliding-window reservations keep concurrent and reentrant submissions from
// spending the same slot. Completed entries expire at the window boundary;
// in-flight entries remain reserved until their provider call returns.
template <typename CacheMap>
class FrequencyWindow
{
public:
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;
    using Time = uint64_t;

    static Time now()
    {
        return static_cast<Time>(std::chrono::duration_cast<Milliseconds>(
            Clock::now().time_since_epoch()).count());
    }

    bool available(const std::string& code, uint32_t limit, uint32_t seconds, Time current = now())
    {
        std::lock_guard<std::mutex> lock(_mutex);
        prune(current);
        if (limit == 0 || seconds == 0)
            return true;
        return count(code) < limit;
    }

    class Reservation
    {
    public:
        Reservation(FrequencyWindow& window, const std::string& code,
                    uint32_t limit, uint32_t seconds, Time current = now())
            : _window(window), _code(code), _id(0), _allowed(false), _submitted(false)
        {
            std::lock_guard<std::mutex> lock(_window._mutex);
            _window.prune(current);
            if (limit == 0 || seconds == 0) {
                _allowed = true;
                return;
            }
            if (_window.count(code) >= limit)
                return;
            // Allocate committed storage before calling the provider, so
            // recording an accepted or uncertain request cannot allocate.
            auto& pending = _window._pending[code];
            auto& times = _window.cache[code];
            auto& spans = _window._durations[code];
            const size_t capacity = times.size() + pending.size() + 1;
            times.reserve(capacity);
            spans.reserve(capacity);
            const uint64_t id = ++_window._next_id;
            pending.push_back({id, current, uint64_t(seconds) * 1000});
            _id = id;
            _allowed = true;
        }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        ~Reservation() { finish(_submitted); }
        explicit operator bool() const { return _allowed; }
        void beginSubmission(Time current = now())
        {
            if (_id != 0) {
                std::lock_guard<std::mutex> lock(_window._mutex);
                auto it = _window._pending.find(_code);
                if (it != _window._pending.end()) {
                    for (auto& entry : it->second)
                        if (entry.id == _id) { entry.start = current; break; }
                }
            }
            _submitted = true;
        }
        void commit() { finish(true); }
        void rollback() { finish(false); }

    private:
        void finish(bool success)
        {
            if (_id == 0)
                return;
            std::lock_guard<std::mutex> lock(_window._mutex);
            auto it = _window._pending.find(_code);
            if (it != _window._pending.end()) {
                auto& entries = it->second;
                auto entry = std::find_if(entries.begin(), entries.end(),
                    [this](const Pending& item) { return item.id == _id; });
                if (entry != entries.end()) {
                    if (success) {
                        _window.cache.find(_code)->second.push_back(entry->start);
                        _window._durations.find(_code)->second.push_back(entry->duration);
                    }
                    entries.erase(entry);
                }
                if (entries.empty()) {
                    _window._pending.erase(it);
                    auto cached = _window.cache.find(_code);
                    if (cached != _window.cache.end() && cached->second.empty()) {
                        _window.cache.erase(cached);
                        _window._durations.erase(_code);
                    }
                }
            }
            _id = 0;
        }
        FrequencyWindow& _window;
        std::string _code;
        uint64_t _id;
        bool _allowed;
        bool _submitted;
    };

    // Exposed for existing diagnostic users; all runtime access goes through
    // the synchronized methods above.
    CacheMap cache;

private:
    struct Pending { uint64_t id; Time start; Time duration; };
    size_t count(const std::string& code) const
    {
        auto committed = cache.find(code);
        auto pending = _pending.find(code);
        return (committed == cache.end() ? 0 : committed->second.size()) +
               (pending == _pending.end() ? 0 : pending->second.size());
    }
    void prune(Time current)
    {
        for (auto it = cache.begin(); it != cache.end();) {
            auto durations = _durations.find(it->first);
            auto& times = it->second;
            if (durations != _durations.end()) {
                auto& spans = durations->second;
                size_t out = 0;
                for (size_t i = 0; i < times.size(); ++i) {
                    if (i >= spans.size()) continue;
                    if (current < times[i] || current - times[i] < spans[i]) {
                        times[out] = times[i];
                        spans[out++] = spans[i];
                    }
                }
                times.resize(out);
                spans.resize(out);
                if (spans.empty() && _pending.find(it->first) == _pending.end())
                    _durations.erase(durations);
            }
            if (times.empty() && _pending.find(it->first) == _pending.end())
                it = cache.erase(it);
            else
                ++it;
        }
    }
    std::mutex _mutex;
    uint64_t _next_id = 0;
    std::unordered_map<std::string, std::vector<Pending>> _pending;
    std::unordered_map<std::string, std::vector<Time>> _durations;
};
