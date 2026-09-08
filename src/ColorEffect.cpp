#include "ColorEffect.h"
#include "WinUtil.h"
#include <magnification.h>
#include <cstring>

namespace paper {
namespace {

struct RestoreData {
    LONG active = 0;
    MAGCOLOREFFECT original{};
    MAGCOLOREFFECT applied{};
};

bool SameMatrix(const MAGCOLOREFFECT& a, const MAGCOLOREFFECT& b) noexcept {
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (std::abs(a.transform[row][column] - b.transform[row][column]) > 0.00001f) return false;
        }
    }
    return true;
}

class MappedView {
public:
    ~MappedView() { if (data) UnmapViewOfFile(data); }
    RestoreData* data = nullptr;
};

}

struct ColorEffect::Impl {
    bool initialized = false;
    bool applied = false;
    winrt::handle mapping;
    winrt::handle stop;
    winrt::handle process;
    MappedView view;

    ~Impl() {
        if (stop && (!view.data || !view.data->active)) SetEvent(stop.get());
        if (initialized) MagUninitialize();
    }

    void Initialize() {
        if (initialized) return;
        CheckWin32(MagInitialize(), L"Windows Magnification is unavailable. Run the native ARM64 or x64 build, not an emulated build.");
        initialized = true;
    }

    void CreateGuardian() {
        if (mapping) return;
        MAGCOLOREFFECT original{};
        CheckWin32(MagGetFullscreenColorEffect(&original), L"Cannot read the existing desktop color transform.");
        const std::wstring name = L"Local\\PaperShade.Restore." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        mapping.attach(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RestoreData), name.c_str()));
        CheckWin32(static_cast<bool>(mapping), L"Cannot allocate color-restore state.");
        view.data = static_cast<RestoreData*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RestoreData)));
        CheckWin32(view.data != nullptr, L"Cannot map color-restore state.");
        *view.data = RestoreData{};
        view.data->original = original;
        stop.attach(CreateEventW(nullptr, TRUE, FALSE, (name + L".Stop").c_str()));
        winrt::handle ready(CreateEventW(nullptr, TRUE, FALSE, (name + L".Ready").c_str()));
        CheckWin32(static_cast<bool>(stop) && static_cast<bool>(ready), L"Cannot create color-restore signals.");
        const auto exe = ExecutablePath();
        std::wstring command = L"\"" + exe + L"\" --guardian " +
            std::to_wstring(GetCurrentProcessId()) + L" \"" + name + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        CheckWin32(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info), L"Cannot start the crash-recovery helper.");
        process.attach(info.hProcess);
        winrt::handle thread(info.hThread);
        const HANDLE wait[]{ready.get(), process.get()};
        const auto result = WaitForMultipleObjects(2, wait, FALSE, 5000);
        if (result != WAIT_OBJECT_0) {
            SetEvent(stop.get());
            throw winrt::hresult_error(E_FAIL, L"Crash recovery could not initialize. No display filter was applied.");
        }
    }
};

ColorEffect::ColorEffect() : impl_(std::make_unique<Impl>()) {}

ColorEffect::~ColorEffect() {
    try {
        Restore();
    } catch (const winrt::hresult_error& error) {
        Log(L"Normal color restoration failed; the guardian will retry on exit: " + std::wstring(error.message()));
    }
}

bool ColorEffect::Active() const noexcept {
    return impl_->applied;
}

void ColorEffect::Apply(Preset preset, std::uint32_t kelvin) {
    if (!ValidKelvin(kelvin)) throw winrt::hresult_invalid_argument(L"Color temperature must be 1000-6500 K.");
    if (UsesCapture(preset, kelvin)) throw winrt::hresult_invalid_argument(L"This style and temperature require the capture engine.");
    impl_->Initialize();
    impl_->CreateGuardian();
    auto& data = *impl_->view.data;
    if (impl_->applied) {
        MAGCOLOREFFECT current{};
        CheckWin32(MagGetFullscreenColorEffect(&current), L"Cannot read the active desktop color transform.");
        if (!SameMatrix(current, data.applied)) {
            impl_->applied = false;
            InterlockedExchange(&data.active, 0);
            throw winrt::hresult_error(E_FAIL, L"Another application changed the desktop color transform. PaperShade paused rather than overwriting it.");
        }
    }
    const auto matrix = ColorMatrix(preset, kelvin);
    const auto previous = data.applied;
    const LONG wasActive = data.active;
    std::memcpy(&data.applied, matrix.data(), sizeof(data.applied));
    InterlockedExchange(&data.active, 1);
    if (!MagSetFullscreenColorEffect(&data.applied)) {
        const DWORD error = GetLastError();
        data.applied = previous;
        InterlockedExchange(&data.active, wasActive);
        throw winrt::hresult_error(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE),
            L"Windows could not apply the grayscale transform.");
    }
    impl_->applied = true;
}

void ColorEffect::Restore() {
    if (impl_->view.data && impl_->view.data->active) {
        auto& data = *impl_->view.data;
        MAGCOLOREFFECT current{};
        CheckWin32(MagGetFullscreenColorEffect(&current), L"Cannot read desktop colors for restoration.");
        if (SameMatrix(current, data.applied)) {
            CheckWin32(MagSetFullscreenColorEffect(&data.original), L"Cannot restore the previous desktop colors.");
        } else {
            Log(L"Another application's color transform was left unchanged.");
        }
        InterlockedExchange(&data.active, 0);
    }
    impl_->applied = false;
    impl_ = std::make_unique<Impl>();
}

int ColorEffect::Guardian(unsigned long parentId, const std::wstring& mappingName) {
    winrt::handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str()));
    winrt::handle parent(OpenProcess(SYNCHRONIZE, FALSE, parentId));
    winrt::handle stop(OpenEventW(SYNCHRONIZE, FALSE, (mappingName + L".Stop").c_str()));
    winrt::handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, (mappingName + L".Ready").c_str()));
    if (!mapping || !parent || !stop || !ready) {
        Log(L"Crash-recovery helper could not open its parent or state.");
        return 1;
    }
    MappedView view;
    view.data = static_cast<RestoreData*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RestoreData)));
    if (!view.data) {
        Log(L"Crash-recovery helper could not map its state.");
        return 1;
    }
    if (!SetEvent(ready.get())) return 1;
    const HANDLE handles[]{parent.get(), stop.get()};
    const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 && view.data->active) {
        if (!MagInitialize()) {
            Log(L"Crash-recovery helper could not initialize Magnification.");
            return 1;
        }
        MAGCOLOREFFECT current{};
        const bool read = MagGetFullscreenColorEffect(&current) != FALSE;
        const bool restored = read && (!SameMatrix(current, view.data->applied) ||
            MagSetFullscreenColorEffect(&view.data->original));
        MagUninitialize();
        if (!restored) {
            Log(L"Crash-recovery helper could not restore desktop colors.");
            return 1;
        }
    } else if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1) {
        Log(L"Crash-recovery helper wait failed.");
        return 1;
    }
    return 0;
}

}
