#pragma once

#include "Filters.h"
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace paper {

inline constexpr UINT CaptureFrameMessage = WM_APP + 10;
inline constexpr UINT CaptureFailureMessage = WM_APP + 11;
inline constexpr UINT CaptureBorderMessage = WM_APP + 12;
inline constexpr UINT_PTR CaptureTimer = 20;

enum class CaptureBorderState { CheckingPermission, Borderless, RequiredByWindows, VisibleByChoice };

struct CaptureStats {
    std::uint64_t frames = 0;
    std::uint32_t monitors = 0;
    bool sourceRateLimited = false;
    CaptureBorderState border = CaptureBorderState::CheckingPermission;
    std::wstring adapter;
};

class CaptureEngine {
public:
    CaptureEngine();
    ~CaptureEngine();
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // All public methods run on the owner window's message-loop thread.
    // Capture callbacks only post coalesced notifications; never access UI or D3D.
    void Start(HWND owner, const Settings& settings);
    void Stop() noexcept;
    void HandleFrame();
    void HandleTimer();
    CaptureStats Stats() const;
    std::wstring LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
