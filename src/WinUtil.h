#pragma once

#include <Windows.h>
#include <winrt/base.h>
#include <string>
#include <vector>

namespace paper {

inline void CheckWin32(BOOL success, const wchar_t* operation) {
    if (!success) {
        const DWORD error = GetLastError();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE), operation);
    }
}

inline std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    CheckWin32(length != 0 && length < buffer.size(), L"Cannot locate PaperShade.exe.");
    return {buffer.data(), length};
}

inline void Log(const std::wstring& message) noexcept {
    OutputDebugStringW(L"PaperShade: ");
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");
}

}
