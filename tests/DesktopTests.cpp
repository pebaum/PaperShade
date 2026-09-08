#include "CaptureEngine.h"
#include "ColorEffect.h"
#include "WinUtil.h"
#include <magnification.h>
#include <psapi.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

paper::CaptureEngine* engine = nullptr;
std::wstring captureError;
bool running = false;
unsigned paintSequence = 0;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Same(const MAGCOLOREFFECT& a, const MAGCOLOREFFECT& b) {
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (std::abs(a.transform[row][column] - b.transform[row][column]) > 0.00001f) return false;
        }
    }
    return true;
}

MAGCOLOREFFECT SnapshotColors() {
    paper::CheckWin32(MagInitialize(), L"Cannot initialize a color-state snapshot.");
    MAGCOLOREFFECT result{};
    const BOOL read = MagGetFullscreenColorEffect(&result);
    const DWORD error = GetLastError();
    MagUninitialize();
    if (!read) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE),
            L"Cannot read the desktop color-state snapshot.");
    }
    return result;
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    try {
        if (message == paper::CaptureFrameMessage && running) {
            engine->HandleFrame();
            return 0;
        }
        if (message == paper::CaptureFailureMessage && running) {
            captureError = engine->LastError();
            return 0;
        }
        if (message == WM_TIMER && wparam == paper::CaptureTimer && running) {
            engine->HandleTimer();
            return 0;
        }
        if (message == WM_TIMER && wparam == 30) {
            ++paintSequence;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT area{};
            GetClientRect(hwnd, &area);
            const auto brush = CreateSolidBrush(RGB((paintSequence * 31) & 255, 130, 230));
            FillRect(dc, &area, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, L"PaperShade display-engine exercise\nColors restore automatically in a few seconds.",
                -1, &area, DT_CENTER | DT_WORDBREAK);
            EndPaint(hwnd, &paint);
            return 0;
        }
    } catch (const winrt::hresult_error& error) {
        captureError = error.message().c_str();
    } catch (const std::exception& error) {
        captureError = winrt::to_hstring(error.what());
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Pump(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline && captureError.empty()) {
        const auto remaining = static_cast<DWORD>(deadline - GetTickCount64());
        MsgWaitForMultipleObjectsEx(0, nullptr, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!captureError.empty()) throw winrt::hresult_error(E_FAIL, captureError);
}

std::uint64_t CpuTime() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    paper::CheckWin32(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user), L"Cannot read CPU time.");
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

struct OverlayInfo {
    HWND host;
    unsigned count = 0;
    bool safe = true;
};

BOOL CALLBACK InspectOverlay(HWND hwnd, LPARAM parameter) {
    auto& info = *reinterpret_cast<OverlayInfo*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId() || hwnd == info.host || !IsWindowVisible(hwnd)) return TRUE;
    const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const auto required = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    DWORD affinity = 0;
    info.safe = info.safe && (style & required) == required &&
        GetWindowDisplayAffinity(hwnd, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE;
    ++info.count;
    return TRUE;
}

void TestCrashRecovery(const MAGCOLOREFFECT& original) {
    const auto path = paper::ExecutablePath();
    std::wstring command = L"\"" + path + L"\" --crash-child";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    paper::CheckWin32(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info), L"Cannot start recovery exercise.");
    winrt::handle process(info.hProcess), thread(info.hThread);
    const auto wait = WaitForSingleObject(process.get(), 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), 99);
        throw std::runtime_error("Recovery exercise child did not finish.");
    }
    DWORD exit = 0;
    GetExitCodeProcess(process.get(), &exit);
    Require(exit == 42, "Recovery exercise child did not reach simulated failure.");
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto current = SnapshotColors();
        if (Same(original, current)) {
            std::cout << "Abnormal-exit recovery restored the original color transform.\n";
            return;
        }
        Sleep(50);
    }
    throw std::runtime_error("Abnormal-exit recovery did not restore original colors within five seconds.");
}

}

int wmain(int argc, wchar_t** argv) {
    HWND host = nullptr;
    try {
        paper::CheckWin32(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            L"Cannot set physical-pixel DPI awareness for the desktop exercise.");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (argc == 4 && std::wstring(argv[1]) == L"--guardian") {
            return paper::ColorEffect::Guardian(std::wcstoul(argv[2], nullptr, 10), argv[3]);
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--crash-child") {
            paper::ColorEffect effect;
            effect.Apply(paper::Preset::Natural);
            Sleep(300);
            TerminateProcess(GetCurrentProcess(), 42);
            return 42;
        }
        const bool captureOnly = argc == 2 && std::wstring(argv[1]) == L"--capture-only";
        const bool warmthOnly = argc == 2 && std::wstring(argv[1]) == L"--warmth-only";
        if (!captureOnly && !warmthOnly && (argc != 2 || std::wstring(argv[1]) != L"--exercise")) {
            std::cout << "Use --exercise, --capture-only, or --warmth-only for the corresponding live desktop checks. "
                         "This is opt-in and is not part of automatic CTest runs.\n";
            return 2;
        }
        const auto original = SnapshotColors();
        if (warmthOnly) {
            paper::ColorEffect effect;
            for (const auto preset : {paper::Preset::Original, paper::Preset::Natural}) {
                effect.Apply(preset, 3500);
                MAGCOLOREFFECT expected{}, actual{};
                const auto matrix = paper::ColorMatrix(preset, 3500);
                std::memcpy(&expected, matrix.data(), sizeof(expected));
                paper::CheckWin32(MagGetFullscreenColorEffect(&actual), L"Cannot read the applied warm matrix.");
                Require(Same(expected, actual), "The compositor did not retain the Kelvin matrix.");
                Sleep(500);
            }
            effect.Restore();
            Require(Same(SnapshotColors(), original), "Warmth pause did not restore the starting transform.");
            std::cout << "Native 3500 K original-color/grayscale matrices and restoration passed.\n";
            return 0;
        }
        if (!captureOnly) {
            paper::ColorEffect effect;
            effect.Apply(paper::Preset::Natural);
            MAGCOLOREFFECT actual{}, expected{};
            const auto matrix = paper::ColorMatrix(paper::Preset::Natural);
            std::memcpy(&expected, matrix.data(), sizeof(expected));
            paper::CheckWin32(MagGetFullscreenColorEffect(&actual), L"Cannot read applied grayscale.");
            Require(Same(actual, expected), "The real compositor did not accept the grayscale matrix.");
            const auto start = CpuTime();
            Sleep(2000);
            std::cout << "Compositor grayscale idle: " << (CpuTime() - start) / 10000.0 << " CPU ms / 2000 ms wall time.\n";
            effect.Restore();
            actual = SnapshotColors();
            Require(Same(actual, original), "Normal pause did not restore the pre-existing transform.");
        }
        if (!captureOnly) TestCrashRecovery(original);
        WNDCLASSW cls{};
        cls.lpfnWndProc = HostProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"PaperShade.DesktopExercise";
        paper::CheckWin32(RegisterClassW(&cls) != 0, L"Cannot register desktop exercise window.");
        host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            cls.lpszClassName, L"PaperShade desktop exercise", WS_POPUP,
            40, 40, 420, 100, nullptr, nullptr, cls.hInstance, nullptr);
        paper::CheckWin32(host != nullptr, L"Cannot create desktop exercise window.");
        ShowWindow(host, SW_SHOWNOACTIVATE);
        {
            paper::CaptureEngine capture;
            engine = &capture;
            paper::Settings settings;
            settings.enabled = true;
            settings.fps = 15;
            settings.preset = paper::Preset::Ink4;
            capture.Start(host, settings);
            capture.Stop();
            Pump(200);
            OverlayInfo canceled{host};
            EnumWindows(InspectOverlay, reinterpret_cast<LPARAM>(&canceled));
            Require(canceled.count == 0 && capture.Stats().frames == 0,
                "A canceled permission request restarted capture or left an overlay behind.");
            for (const auto preset : {paper::Preset::Ink4, paper::Preset::Ps1Color}) {
                settings.preset = preset;
                capture.Start(host, settings);
                running = true;
                paper::CheckWin32(SetTimer(host, 30, 60, nullptr) != 0, L"Cannot animate desktop exercise.");
                const auto start = GetTickCount64();
                Pump(3000);
                KillTimer(host, 30);
                const auto stats = capture.Stats();
                std::wcout << paper::PresetNames[static_cast<unsigned>(preset)] << L": "
                    << stats.frames << L" presented frames / " << GetTickCount64() - start
                    << L" ms, monitors=" << stats.monitors << L", adapter=" << stats.adapter
                    << L", source-rate-cap=" << stats.sourceRateLimited
                    << L", borderless-allowed-and-requested="
                    << (stats.border == paper::CaptureBorderState::Borderless) << L"\n";
                Require(stats.frames > 1, "No live desktop frames were presented.");
                if (captureOnly) Require(stats.border == paper::CaptureBorderState::Borderless,
                    "Windows did not allow borderless capture, or IsBorderRequired(false) was not retained.");
                Require(stats.frames <= stats.monitors * 50ull, "15 fps cap exceeded its bounded scheduling tolerance.");
                OverlayInfo overlay{host};
                EnumWindows(InspectOverlay, reinterpret_cast<LPARAM>(&overlay));
                Require(overlay.safe && overlay.count == stats.monitors, "Not all overlays are click-through, nonactivating, and capture-excluded.");
                Require(WindowFromPoint(POINT{80, 80}) == host, "The display overlay intercepts input to the underlying window.");
                running = false;
                capture.Stop();
                OverlayInfo stopped{host};
                EnumWindows(InspectOverlay, reinterpret_cast<LPARAM>(&stopped));
                Require(stopped.count == 0, "An overlay remained visible after Stop.");
                Pump(100);
            }
            settings.hideCaptureIndicator = false;
            capture.Start(host, settings);
            running = true;
            Pump(500);
            Require(capture.Stats().border == paper::CaptureBorderState::VisibleByChoice,
                "The visible capture-indicator preference was not honored.");
            Require(capture.Stats().frames > 0, "Capture did not start with the indicator enabled.");
            running = false;
            capture.Stop();
            engine = nullptr;
        }
        DestroyWindow(host);
        host = nullptr;
        const auto finalEffect = SnapshotColors();
        Require(Same(finalEffect, original), "Desktop exercise did not restore the starting display colors.");
        std::cout << "Requested desktop exercises passed, including border preference and permission-cancellation cleanup.\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << winrt::to_string(error.message()) << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
    }
    if (host) DestroyWindow(host);
    return 1;
}
