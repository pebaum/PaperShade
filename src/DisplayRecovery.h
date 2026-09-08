#pragma once

#include <algorithm>
#include <cstdint>

namespace paper {

class DisplayRecovery {
public:
    static constexpr std::uint64_t SettleMilliseconds = 700;
    static constexpr unsigned MaximumAttempts = 3;

    void DisplayChanged(std::uint64_t now) noexcept {
        attempts_ = 0;
        pending_ = true;
        due_ = now + SettleMilliseconds;
    }

    bool Retry(std::uint64_t now) noexcept {
        if (attempts_ >= MaximumAttempts) {
            pending_ = false;
            due_ = 0;
            return false;
        }
        pending_ = true;
        due_ = now + SettleMilliseconds * (attempts_ + 1);
        return true;
    }

    bool Begin(std::uint64_t now) noexcept {
        if (!pending_ || now < due_) return false;
        pending_ = false;
        ++attempts_;
        return true;
    }

    void Complete() noexcept { Cancel(); }
    void Cancel() noexcept { pending_ = false; attempts_ = 0; due_ = 0; }
    bool Pending() const noexcept { return pending_; }
    bool Exhausted() const noexcept { return !pending_ && attempts_ >= MaximumAttempts; }
    unsigned Attempts() const noexcept { return attempts_; }
    std::uint32_t Delay(std::uint64_t now) const noexcept {
        return static_cast<std::uint32_t>(due_ > now ? std::min<std::uint64_t>(due_ - now, 5000) : 1);
    }

private:
    bool pending_ = false;
    unsigned attempts_ = 0;
    std::uint64_t due_ = 0;
};

}
