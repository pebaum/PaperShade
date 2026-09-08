#include "CaptureEngine.h"
#include "ColorEffect.h"
#include "Settings.h"
#include "WinUtil.h"
#include "AppVersion.h"
#include "WarmthDialog.h"
#include "DisplayRecovery.h"
#include <dbt.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <powersetting.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <sstream>
#include <deque>
#include <utility>

namespace paper {
namespace {

constexpr wchar_t WindowClass[] = L"PaperShade.TrayHost";
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT CommandMessage = WM_APP + 2;
constexpr UINT QueryMessage = WM_APP + 3;
constexpr UINT DeferredWorkMessage = WM_APP + 4;
constexpr UINT_PTR RestartTimer = 1;
constexpr UINT Toggle = 100;
constexpr UINT Pause = 101;
constexpr UINT Enable = 102;
constexpr UINT Startup = 103;
constexpr UINT About = 104;
constexpr UINT Quit = 105;
constexpr UINT ResetSettings = 106;
constexpr UINT CaptureIndicator = 107;
constexpr UINT CustomTemperature = 108;
constexpr UINT WarmthOnly = 109;
constexpr UINT TemperatureBase = 10000;
constexpr UINT PresetBase = 200;
constexpr UINT FpsBase = 300;
constexpr UINT SizeBase = 400;
constexpr int ToggleHotkey = 1;
constexpr int PanicHotkey = 2;
constexpr std::array<std::uint32_t, 4> FrameCaps{10, 15, 30, 60};
constexpr std::array<const wchar_t*, 13> PresetKeys{
    L"natural", L"classic", L"average", L"paper", L"contrast", L"inverted",
    L"ink-crisp", L"ink-dither", L"ink4", L"ink16", L"ps1-gray", L"ps1-color", L"original"
};

HICON CreateTrayIcon(bool enabled) {
    constexpr int size = 32;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    CheckWin32(color != nullptr, L"Cannot create tray icon.");
    auto* pixels = static_cast<DWORD*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            DWORD pixel = 0;
            if (x >= 3 && x < 29 && y >= 2 && y < 30) {
                const bool edge = x == 3 || x == 28 || y == 2 || y == 29;
                const bool ink = (x / 3 + y / 3) % 2 == 0 && x + y > 28;
                pixel = edge || (enabled && ink) ? 0xff252525 : 0xffededed;
                if (!enabled && y > 14 && y < 18 && x > 8 && x < 24) pixel = 0xff777777;
            }
            pixels[y * size + x] = pixel;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        CheckWin32(FALSE, L"Cannot create tray icon mask.");
    }
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    CheckWin32(icon != nullptr, L"Cannot initialize tray icon.");
    return icon;
}

class App {
public:
    explicit App(Settings settings) : settings_(settings) {}
    ~App() {
        closing_ = true;
        engineBusy_ = true;
        capture_.Stop();
        if (power_) UnregisterPowerSettingNotification(power_);
        if (monitorNotifications_) UnregisterDeviceNotification(monitorNotifications_);
        if (adapterNotifications_) UnregisterDeviceNotification(adapterNotifications_);
        if (hwnd_) {
            UnregisterHotKey(hwnd_, ToggleHotkey);
            UnregisterHotKey(hwnd_, PanicHotkey);
            WTSUnRegisterSessionNotification(hwnd_);
            NOTIFYICONDATAW icon{sizeof(icon)};
            icon.hWnd = hwnd_;
            icon.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &icon);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
        if (onIcon_) DestroyIcon(onIcon_);
        if (offIcon_) DestroyIcon(offIcon_);
    }

    void Initialize(HINSTANCE instance) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = instance;
        cls.lpszClassName = WindowClass;
        CheckWin32(RegisterClassExW(&cls) != 0, L"Cannot register the tray host.");
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, L"PaperShade",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        CheckWin32(hwnd_ != nullptr, L"Cannot create the tray host.");
#ifdef PAPERSHADE_APP_TESTS
        if (testing_) return;
#endif
        onIcon_ = CreateTrayIcon(true);
        offIcon_ = CreateTrayIcon(false);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        CheckWin32(taskbarCreated_ != 0, L"Cannot subscribe to taskbar recovery.");
        AddTray();
        CheckWin32(RegisterHotKey(hwnd_, PanicHotkey,
            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'G'),
            L"Ctrl+Alt+Shift+G is already registered. Close the conflicting hotkey app before starting PaperShade.");
        if (!RegisterHotKey(hwnd_, ToggleHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G')) {
            Notify(L"Toggle shortcut unavailable", L"Ctrl+Alt+G is in use. Use the tray icon to toggle; Ctrl+Alt+Shift+G still pauses.");
        }
        CheckWin32(WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION),
            L"Cannot subscribe to Windows session-lock notifications.");
        power_ = RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
        CheckWin32(power_ != nullptr, L"Cannot subscribe to display power notifications.");
        DEV_BROADCAST_DEVICEINTERFACE_W deviceFilter{};
        deviceFilter.dbcc_size = sizeof(deviceFilter);
        deviceFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        deviceFilter.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
        monitorNotifications_ = RegisterDeviceNotificationW(hwnd_, &deviceFilter, DEVICE_NOTIFY_WINDOW_HANDLE);
        CheckWin32(monitorNotifications_ != nullptr, L"Cannot subscribe to monitor connection notifications.");
        deviceFilter.dbcc_classguid = GUID_DEVINTERFACE_DISPLAY_ADAPTER;
        adapterNotifications_ = RegisterDeviceNotificationW(hwnd_, &deviceFilter, DEVICE_NOTIFY_WINDOW_HANDLE);
        CheckWin32(adapterNotifications_ != nullptr, L"Cannot subscribe to display-adapter notifications.");
        topology_ = ReadTopology();
        Apply();
    }

    void Command(UINT command) {
        if (closing_) return;
        if (command == Quit) {
            Shutdown();
            return;
        }
        if (command == About) {
            ShowAbout();
            return;
        }
        if (command == Startup) {
            SetStartsWithWindows(!StartsWithWindows());
            return;
        }
        if (command == CustomTemperature) {
            const auto selected = ShowWarmthDialog(hwnd_, settings_.temperatureKelvin);
            if (selected) Command(TemperatureBase + *selected);
            return;
        }
        if (command == ResetSettings) {
            settings_ = Settings{};
        } else if (command == Toggle) {
            settings_.enabled = !settings_.enabled;
        } else if (command == Pause) {
            settings_.enabled = false;
        } else if (command == Enable) {
            settings_.enabled = true;
        } else if (command == CaptureIndicator) {
            settings_.hideCaptureIndicator = !settings_.hideCaptureIndicator;
        } else if (command == WarmthOnly) {
            settings_.preset = Preset::Original;
            if (settings_.temperatureKelvin == NeutralKelvin) settings_.temperatureKelvin = 3500;
            settings_.enabled = true;
        } else if (command >= TemperatureBase + MinimumKelvin && command <= TemperatureBase + NeutralKelvin) {
            settings_.temperatureKelvin = command - TemperatureBase;
            if (settings_.temperatureKelvin != NeutralKelvin) settings_.enabled = true;
        } else if (command >= PresetBase && command < PresetBase + PresetNames.size()) {
            settings_.preset = static_cast<Preset>(command - PresetBase);
            settings_.enabled = true;
        } else if (command >= FpsBase && command < FpsBase + FrameCaps.size()) {
            settings_.fps = FrameCaps[command - FpsBase];
        } else if (command >= SizeBase && command < SizeBase + 4) {
            settings_.pixelSize = command - SizeBase + 1;
        } else {
            throw winrt::hresult_invalid_argument(L"Unknown PaperShade command.");
        }
        if (recovery_.Pending() && CanRun()) {
            UpdateTray();
        } else {
            Apply();
        }
        PersistSettings();
    }

    void Fail(const std::wstring& message) noexcept {
        EngineScope operation(*this);
        recovery_.Cancel();
        recoveryNeedsStop_ = false;
        KillTimer(hwnd_, RestartTimer);
        capture_.Stop();
        ++errors_;
        settings_.enabled = false;
        std::wstring detail = message;
        try {
            color_.Restore();
        } catch (const winrt::hresult_error& error) {
            detail += L"\nColor restoration: " + std::wstring(error.message()) +
                L"\nExit PaperShade to let the recovery helper retry.";
        }
        try {
            PersistSettings();
        } catch (const winrt::hresult_error& error) {
            detail += L"\nPreferences: " + std::wstring(error.message());
        }
        Log(detail);
        UpdateTray();
        Notify(L"PaperShade paused", detail);
    }

    HWND Window() const noexcept { return hwnd_; }

private:
    struct PendingMessage {
        UINT message;
        WPARAM wparam;
        LPARAM lparam;
    };

    struct EngineScope {
        App& app;
        explicit EngineScope(App& owner) noexcept : app(owner) {
            app.engineBusy_ = true;
#ifdef PAPERSHADE_APP_TESTS
            ++app.testEngineEntries_;
#endif
        }
        ~EngineScope() {
            app.engineBusy_ = false;
            app.PostDeferredWork();
        }
    };

    HWND hwnd_ = nullptr;
    HICON onIcon_ = nullptr;
    HICON offIcon_ = nullptr;
    HPOWERNOTIFY power_ = nullptr;
    HDEVNOTIFY monitorNotifications_ = nullptr;
    HDEVNOTIFY adapterNotifications_ = nullptr;
    UINT taskbarCreated_ = 0;
    Settings settings_;
    CaptureEngine capture_;
    ColorEffect color_;
    bool locked_ = false;
    bool displayOff_ = false;
    bool suspended_ = false;
    bool closing_ = false;
    bool borderNoticeShown_ = false;
    std::uint32_t errors_ = 0;
    bool engineBusy_ = false;
    bool deferredPosted_ = false;
    bool deferredFrame_ = false;
    bool deferredCaptureTimer_ = false;
    bool deferredFailure_ = false;
    bool deferredBorder_ = false;
    bool deferredRestart_ = false;
    bool recoveryNeedsStop_ = false;
    std::deque<PendingMessage> deferredMessages_;
    DisplayRecovery recovery_;
    std::array<int, 5> topology_{};

#ifdef PAPERSHADE_APP_TESTS
    bool testing_ = false;
    unsigned testEngineEntries_ = 0;
public:
    void InitializeForTest(HINSTANCE instance) { testing_ = true; Initialize(instance); }
    template<typename Callback> void TestCriticalSection(Callback callback) {
        EngineScope operation(*this);
        callback();
    }
    bool TestRecoveryPending() const { return recovery_.Pending(); }
    unsigned TestErrors() const { return errors_; }
    CaptureStats TestCaptureStats() const { return capture_.Stats(); }
    unsigned TestEngineEntries() const { return testEngineEntries_; }
private:
#endif

    static std::array<int, 5> ReadTopology() {
        return {GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }

    void PersistSettings() {
#ifdef PAPERSHADE_APP_TESTS
        if (testing_) return;
#endif
        SaveSettings(settings_);
    }

    void PostDeferredWork() noexcept {
        if (!hwnd_ || closing_ || engineBusy_ || deferredPosted_) return;
        if (!recoveryNeedsStop_ && !deferredRestart_ && !deferredFrame_ &&
            !deferredCaptureTimer_ && !deferredFailure_ && !deferredBorder_ && deferredMessages_.empty()) return;
        if (PostMessageW(hwnd_, DeferredWorkMessage, 0, 0)) {
            deferredPosted_ = true;
        } else {
            Log(L"Could not post deferred display work.");
        }
    }

    bool DeferDuringEngineCall(UINT message, WPARAM wparam, LPARAM lparam) {
        if (!engineBusy_) return false;
        switch (message) {
        case DeferredWorkMessage:
            deferredPosted_ = false;
            return true;
        case CaptureFrameMessage: deferredFrame_ = true; return true;
        case CaptureFailureMessage: deferredFailure_ = true; return true;
        case CaptureBorderMessage: deferredBorder_ = true; return true;
        case WM_TIMER:
            if (wparam == RestartTimer) {
                KillTimer(hwnd_, RestartTimer);
                deferredRestart_ = true;
            } else if (wparam == CaptureTimer) {
                KillTimer(hwnd_, CaptureTimer);
                deferredCaptureTimer_ = true;
            }
            return true;
        case CommandMessage:
        case WM_HOTKEY:
        case TrayMessage:
        case WM_CLOSE:
        case WM_ENDSESSION:
            // These messages contain only scalar values. Device/power payload pointers
            // are consumed immediately by their handlers, never retained in this queue.
            deferredMessages_.push_back({message, wparam, lparam});
            return true;
        default:
            return false;
        }
    }

    bool CanRun() const {
        return settings_.enabled && !locked_ && !displayOff_ && !suspended_ && !closing_;
    }

    bool CanRenderCapture() const {
        return CanRun() && !recovery_.Pending() && !recovery_.Exhausted() && !recoveryNeedsStop_;
    }

    void Apply(bool recovering = false) {
        EngineScope operation(*this);
        KillTimer(hwnd_, RestartTimer);
        if (!recovering) {
            recovery_.Cancel();
            recoveryNeedsStop_ = false;
        }
        capture_.Stop();
        if (!CanRun() || IsNeutralOriginal(settings_.preset, settings_.temperatureKelvin)) {
            color_.Restore();
        } else if (UsesCapture(settings_.preset, settings_.temperatureKelvin)) {
            color_.Restore();
            capture_.Start(hwnd_, settings_);
        } else {
            color_.Apply(settings_.preset, settings_.temperatureKelvin);
        }
        UpdateTray();
    }

    void SuspendOrResume() {
        if (closing_) return;
        topology_ = ReadTopology();
        recovery_.DisplayChanged(GetTickCount64());
        recoveryNeedsStop_ = true;
        KillTimer(hwnd_, RestartTimer);
        PostDeferredWork();
    }

    void RetryDisplay(const std::wstring& reason) {
        if (!CanRun()) return;
        if (!recovery_.Retry(GetTickCount64())) {
            EngineScope operation(*this);
            recoveryNeedsStop_ = false;
            KillTimer(hwnd_, RestartTimer);
            capture_.Stop();
            color_.Restore();
            ++errors_;
            Log(reason);
            UpdateTray();
            if (recovery_.Exhausted()) {
                Notify(L"Waiting for the display",
                    L"The graphics driver is not ready. Your filter preference is preserved. Reconnect the display or choose Retry display capture.");
            }
            return;
        }
        recoveryNeedsStop_ = true;
        KillTimer(hwnd_, RestartTimer);
        PostDeferredWork();
    }

    void ArmRecoveryTimer() {
        if (!CanRun() || closing_) {
            recovery_.Cancel();
            return;
        }
        if (recovery_.Pending()) {
            CheckWin32(SetTimer(hwnd_, RestartTimer, recovery_.Delay(GetTickCount64()), nullptr) != 0,
                L"Cannot schedule display recovery.");
        }
    }

    void RestartDisplay() {
        KillTimer(hwnd_, RestartTimer);
        if (!CanRun() || closing_) {
            recovery_.Cancel();
            return;
        }
        if (recoveryNeedsStop_) {
            PostDeferredWork();
        } else if (recovery_.Begin(GetTickCount64())) {
            Apply(true);
            if (!UsesCapture(settings_.preset, settings_.temperatureKelvin) && !recovery_.Pending()) {
                recovery_.Complete();
            }
        } else {
            ArmRecoveryTimer();
        }
    }

    void DrainDeferredWork() {
        deferredPosted_ = false;
        auto messages = std::move(deferredMessages_);
        deferredMessages_.clear();
        for (const auto& message : messages) {
            Dispatch(message.message, message.wparam, message.lparam);
            if (closing_) return;
        }
        if (recoveryNeedsStop_) {
            recoveryNeedsStop_ = false;
            {
                EngineScope operation(*this);
                capture_.Stop();
                color_.Restore();
            }
            UpdateTray();
            ArmRecoveryTimer();
        }
        if (std::exchange(deferredRestart_, false)) RestartDisplay();
        if (std::exchange(deferredFailure_, false)) Dispatch(CaptureFailureMessage, 0, 0);
        if (std::exchange(deferredBorder_, false)) Dispatch(CaptureBorderMessage, 0, 0);
        if (std::exchange(deferredCaptureTimer_, false)) Dispatch(WM_TIMER, CaptureTimer, 0);
        if (std::exchange(deferredFrame_, false)) Dispatch(CaptureFrameMessage, 0, 0);
        PostDeferredWork();
    }

    NOTIFYICONDATAW TrayData() const {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = hwnd_;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = CanRun() ? onIcon_ : offIcon_;
        const auto name = std::wstring(L"PaperShade - ") +
            (CanRun() ? (recovery_.Exhausted() ? L"waiting for the display" :
                recovery_.Pending() ? L"reconnecting displays" :
                PresetNames[static_cast<std::size_t>(settings_.preset)]) : L"paused") +
            L" | " + std::to_wstring(settings_.temperatureKelvin) + L" K";
        wcsncpy_s(icon.szTip, name.c_str(), _TRUNCATE);
        return icon;
    }

    void AddTray() {
        auto icon = TrayData();
        CheckWin32(Shell_NotifyIconW(NIM_ADD, &icon), L"Cannot add the PaperShade notification-area icon.");
        icon.uVersion = NOTIFYICON_VERSION_4;
        CheckWin32(Shell_NotifyIconW(NIM_SETVERSION, &icon), L"Cannot initialize the notification-area icon.");
    }

    void UpdateTray() noexcept {
        if (!hwnd_ || !onIcon_) return;
        auto icon = TrayData();
        if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) Log(L"Taskbar icon update failed; waiting for Explorer recovery.");
    }

    void Notify(const std::wstring& title, const std::wstring& text) noexcept {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = hwnd_;
        icon.uID = 1;
        icon.uFlags = NIF_INFO;
        icon.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) Log(title + L": " + text);
    }

    void Menu() {
        const bool startup = StartsWithWindows();
        HMENU menu = CreatePopupMenu();
        HMENU presets = CreatePopupMenu();
        HMENU frames = CreatePopupMenu();
        HMENU pixels = CreatePopupMenu();
        HMENU warmth = CreatePopupMenu();
        if (!menu || !presets || !frames || !pixels || !warmth) {
            if (menu) DestroyMenu(menu);
            if (presets) DestroyMenu(presets);
            if (frames) DestroyMenu(frames);
            if (pixels) DestroyMenu(pixels);
            if (warmth) DestroyMenu(warmth);
            throw winrt::hresult_error(E_OUTOFMEMORY, L"Cannot open the tray menu.");
        }
        AppendMenuW(menu, MF_STRING, recovery_.Exhausted() ? Enable : Toggle,
            recovery_.Exhausted() ? L"Retry display capture" :
            CanRun() ? L"Pause filters\tCtrl+Alt+G" : L"Enable filters\tCtrl+Alt+G");
        AppendMenuW(menu, MF_STRING, Pause, L"Emergency pause\tCtrl+Alt+Shift+G");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (UINT index = 0; index < PresetNames.size(); ++index) {
            if (index == 6 || index == 10 || index == 12) AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
            const UINT flags = MF_STRING | (index == static_cast<UINT>(settings_.preset) ? MF_CHECKED : 0);
            AppendMenuW(presets, flags, PresetBase + index, PresetNames[index]);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(presets), L"Display style");
        AppendMenuW(warmth, MF_STRING, WarmthOnly, L"Warmth only (original colors)");
        AppendMenuW(warmth, MF_SEPARATOR, 0, nullptr);
        for (const auto kelvin : KelvinPresets) {
            const std::wstring label = std::to_wstring(kelvin) +
                (kelvin == NeutralKelvin ? L" K - neutral / off" : L" K");
            AppendMenuW(warmth, MF_STRING | (settings_.temperatureKelvin == kelvin ? MF_CHECKED : 0),
                TemperatureBase + kelvin, label.c_str());
        }
        AppendMenuW(warmth, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(warmth, MF_STRING, CustomTemperature, L"Custom temperature...");
        const auto warmthLabel = L"Warmth (Kelvin) - " + std::to_wstring(settings_.temperatureKelvin) + L" K";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(warmth), warmthLabel.c_str());
        for (UINT index = 0; index < FrameCaps.size(); ++index) {
            std::wstring label = std::to_wstring(FrameCaps[index]) + L" fps";
            if (FrameCaps[index] == 15) label += L" - balanced (default)";
            if (FrameCaps[index] == 10) label += L" - lowest power";
            if (FrameCaps[index] == 60) label += L" - smoothest / more GPU";
            AppendMenuW(frames, MF_STRING | (settings_.fps == FrameCaps[index] ? MF_CHECKED : 0),
                FpsBase + index, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(frames), L"Advanced filter frame cap");
        for (UINT size = 1; size <= 4; ++size) {
            const std::wstring label = size == 1 ? L"1 pixel - native / exact PS1 pattern" :
                std::to_wstring(size) + L" pixels - enlarged artistic pattern";
            AppendMenuW(pixels, MF_STRING | (settings_.pixelSize == size ? MF_CHECKED : 0),
                SizeBase + size - 1, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pixels), L"Dither pattern size");
        AppendMenuW(menu, MF_STRING | (settings_.hideCaptureIndicator ? MF_CHECKED : 0),
            CaptureIndicator, L"Hide screen-capture indicator");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), Startup, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, About, L"About / active engine");
        AppendMenuW(menu, MF_STRING, Quit, L"Exit and restore colors");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(hwnd_);
        const auto command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            point.x, point.y, hwnd_, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        if (command) Command(command);
    }

    void ShowAbout() {
        const auto stats = capture_.Stats();
        std::wostringstream text;
        text << L"PaperShade " << PaperShadeVersion << L" - native Windows 11 desktop filters\n\n"
             << L"Style: " << PresetNames[static_cast<std::size_t>(settings_.preset)]
             << L"\nState: " << (CanRun() ? L"enabled" : L"paused")
             << L"\nColor temperature: " << settings_.temperatureKelvin << L" K"
             << (settings_.temperatureKelvin == NeutralKelvin ? L" (neutral / off)\n" : L" (approximate warmth)\n");
        if (CanRun() && UsesCapture(settings_.preset, settings_.temperatureKelvin)) {
            text << L"Engine: GPU capture + one shader pass\nAdapter: " << stats.adapter
                 << L"\nDisplays: " << stats.monitors << L"\nPresented frames: " << stats.frames
                 << L"\nCap: " << settings_.fps << L" fps (maximum, not a fixed redraw loop)"
                 << L"\nCapture-source cap: " << (stats.sourceRateLimited ? L"supported" : L"not available; render-side cap only");
            const wchar_t* border = L"checking Windows permission";
            switch (stats.border) {
            case CaptureBorderState::Borderless: border = L"off for PaperShade (Windows allowed)"; break;
            case CaptureBorderState::RequiredByWindows: border = L"required by Windows / permission denied"; break;
            case CaptureBorderState::VisibleByChoice: border = L"on by preference"; break;
            default: break;
            }
            text << L"\nCapture indicator: " << border;
        } else {
            text << L"Engine: " << (CanRun() && !IsNeutralOriginal(settings_.preset, settings_.temperatureKelvin)
                ? L"Windows compositor color matrix (no app render loop)" : L"idle / no color processing");
        }
        text << L"\nReported errors: " << errors_;
        if (recovery_.Pending()) text << L"\nDisplay recovery: waiting for the monitor layout to settle";
        if (recovery_.Exhausted()) text << L"\nDisplay recovery: waiting for a new display event or Retry display capture";
        const auto captureError = capture_.LastError();
        if (!captureError.empty()) text << L"\nLast capture error: " << captureError;
        text << L"\n\nLeft-click icon: toggle. Right-click: styles and settings."
             << L"\nCtrl+Alt+Shift+G: always pause."
             << L"\n\nAdvanced filters process SDR desktop pixels locally. No recordings, network, or telemetry."
             << L"\nBorderless capture uses Windows permission; another capture app or system policy can still require an indicator."
             << L"\nThe live cursor, secure desktop, protected video, "
                L"and exclusive fullscreen content cannot be guaranteed."
             << L"\nPS1 native mode reproduces signed 4x4 dithering and RGB555 quantization; "
                L"it is not console or CRT emulation."
             << L"\nKelvin warmth is applied after the style and can tint its final palette. 6500 K disables warmth."
             << L"\n\nARM64 devices: use the ARM64 build. Intel/AMD PCs: use x64.";
        MessageBoxW(hwnd_, text.str().c_str(), L"About PaperShade", MB_OK | MB_ICONINFORMATION);
    }

    void Shutdown() {
        EngineScope operation(*this);
        closing_ = true;
        recovery_.Cancel();
        recoveryNeedsStop_ = false;
        KillTimer(hwnd_, RestartTimer);
        capture_.Stop();
        color_.Restore();
        UpdateTray();
        PostQuitMessage(0);
    }

    LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            AddTray();
            return 0;
        }
        switch (message) {
        case DeferredWorkMessage:
            DrainDeferredWork();
            return 0;
        case CommandMessage:
            Command(static_cast<UINT>(wparam));
            return 1;
        case QueryMessage:
            switch (wparam) {
            case 0: return CanRun();
            case 1: return static_cast<LRESULT>(settings_.preset);
            case 2: return settings_.fps;
            case 3: return settings_.pixelSize;
            case 4: return static_cast<LRESULT>(capture_.Stats().frames);
            case 5: return errors_;
            case 6: return static_cast<LRESULT>(capture_.Stats().border);
            case 7: return settings_.temperatureKelvin;
            default: return -1;
            }
        case TrayMessage: {
            const UINT event = LOWORD(lparam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT) Command(Toggle);
            else if (event == WM_CONTEXTMENU) Menu();
            return 0;
        }
        case WM_HOTKEY:
            Command(wparam == PanicHotkey ? Pause : Toggle);
            return 0;
        case CaptureFrameMessage:
            if (CanRenderCapture() && UsesCapture(settings_.preset, settings_.temperatureKelvin)) {
                EngineScope operation(*this);
                capture_.HandleFrame();
                if (!recovery_.Pending() && capture_.Stats().frames > 0) recovery_.Complete();
            }
            return 0;
        case CaptureFailureMessage: {
            const auto detail = capture_.LastError();
            if (CanRun() && UsesCapture(settings_.preset, settings_.temperatureKelvin) && !detail.empty()) {
                if (IsRecoverableDisplayError(capture_.LastFailureCode())) RetryDisplay(detail);
                else Fail(detail);
            }
            return 0;
        }
        case CaptureBorderMessage:
            if (CanRun() && UsesCapture(settings_.preset, settings_.temperatureKelvin)) {
                const bool required = capture_.Stats().border == CaptureBorderState::RequiredByWindows;
                if (required && !borderNoticeShown_) {
                    Notify(L"Windows requires the capture indicator",
                        L"Borderless capture was not allowed on this system. Filtering still works with the border; no system privacy settings were changed.");
                }
                borderNoticeShown_ = required;
            }
            return 0;
        case WM_TIMER:
            if (wparam == RestartTimer) RestartDisplay();
            else if (wparam == CaptureTimer && CanRenderCapture()) {
                EngineScope operation(*this);
                capture_.HandleTimer();
                if (!recovery_.Pending() && capture_.Stats().frames > 0) recovery_.Complete();
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DWMCOMPOSITIONCHANGED:
        case CaptureTopologyMessage:
            SuspendOrResume();
            return 0;
        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVNODES_CHANGED) {
                if (ReadTopology() != topology_) SuspendOrResume();
            } else if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) && lparam) {
                const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lparam);
                if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                    header->dbch_size >= sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)) {
                    const auto* device = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(header);
                    if (IsEqualGUID(device->dbcc_classguid, GUID_DEVINTERFACE_MONITOR) ||
                        IsEqualGUID(device->dbcc_classguid, GUID_DEVINTERFACE_DISPLAY_ADAPTER)) SuspendOrResume();
                }
            }
            return TRUE;
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) SuspendOrResume();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wparam == WTS_SESSION_LOCK || wparam == WTS_SESSION_UNLOCK) {
                locked_ = wparam == WTS_SESSION_LOCK;
                SuspendOrResume();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMSUSPEND || wparam == PBT_APMRESUMEAUTOMATIC) {
                suspended_ = wparam == PBT_APMSUSPEND;
                SuspendOrResume();
            } else if (wparam == PBT_POWERSETTINGCHANGE) {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam);
                if (setting && IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) &&
                    setting->DataLength == sizeof(DWORD)) {
                    DWORD state = 0;
                    std::memcpy(&state, setting->Data, sizeof(state));
                    displayOff_ = state == 0;
                    SuspendOrResume();
                }
            }
            return TRUE;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) Shutdown();
            return 0;
        case WM_CLOSE:
            Shutdown();
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<App*>(create->lpCreateParams);
            app->hwnd_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try {
            if (app->DeferDuringEngineCall(message, wparam, lparam)) {
                return message == CommandMessage ? 1 : 0;
            }
            return app->Dispatch(message, wparam, lparam);
        } catch (const winrt::hresult_error& error) {
            if (IsRecoverableDisplayError(error.code()) && app->CanRun()) app->RetryDisplay(error.message().c_str());
            else app->Fail(error.message().c_str());
        } catch (const std::exception& error) {
            app->Fail(winrt::to_hstring(error.what()).c_str());
        }
        return 0;
    }
};

UINT ParseCommand(int argc, wchar_t** argv) {
    if (argc <= 1) return 0;
    const std::wstring option = argv[1];
    if (argc == 2) {
        if (option == L"--pause" || option == L"--safe") return Pause;
        if (option == L"--enable") return Enable;
        if (option == L"--toggle") return Toggle;
        if (option == L"--quit") return Quit;
        if (option == L"--reset-settings") return ResetSettings;
        if (option == L"--startup") return 0;
        if (option == L"--about") return About;
        if (option == L"--warmth") return CustomTemperature;
        if (option == L"--warmth-only") return WarmthOnly;
    }
    if (argc == 3 && option == L"--preset") {
        for (UINT index = 0; index < PresetKeys.size(); ++index) {
            if (std::wstring(argv[2]) == PresetKeys[index]) return PresetBase + index;
        }
    }
    if (argc == 3 && (option == L"--fps" || option == L"--pixel-size" || option == L"--kelvin")) {
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long value = std::wcstoul(argv[2], &end, 10);
        if (errno == 0 && end != argv[2] && *end == L'\0') {
            if (option == L"--kelvin" && ValidKelvin(value)) return TemperatureBase + value;
            if (option == L"--pixel-size" && value >= 1 && value <= 4) return SizeBase + value - 1;
            for (UINT index = 0; index < FrameCaps.size(); ++index) {
                if (option == L"--fps" && value == FrameCaps[index]) return FpsBase + index;
            }
        }
    }
    throw winrt::hresult_invalid_argument(
        L"Options: --enable, --pause, --toggle, --quit, --safe, --about, --reset-settings, "
        L"--preset <natural|classic|average|paper|contrast|inverted|ink-crisp|ink-dither|ink4|ink16|ps1-gray|ps1-color|original>, "
        L"--fps <10|15|30|60>, --pixel-size <1|2|3|4>, --kelvin <1000..6500>, --warmth, --warmth-only.");
}

}
}

#ifndef PAPERSHADE_APP_TESTS
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace paper;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        int argc = 0;
        wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &argc);
        CheckWin32(raw != nullptr, L"Cannot read launch arguments.");
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(raw, LocalFree);
        if (argc == 4 && std::wstring(raw[1]) == L"--guardian") {
            wchar_t* end = nullptr;
            const auto id = std::wcstoul(raw[2], &end, 10);
            if (!id || !end || *end != 0 || std::wstring(raw[3]).find(L"Local\\PaperShade.Restore.") != 0) return 2;
            return ColorEffect::Guardian(id, raw[3]);
        }
        USHORT processMachine = 0, nativeMachine = 0;
        CheckWin32(IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine),
            L"Cannot determine native Windows architecture.");
        if (processMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            throw winrt::hresult_error(E_FAIL,
                L"PaperShade must run natively for whole-display grayscale. Use ARM64 on Snapdragon/ARM PCs, or x64 on Intel/AMD PCs.");
        }
        const auto command = ParseCommand(argc, raw);
        winrt::handle singleton(CreateMutexW(nullptr, TRUE, L"Local\\PaperShade.Instance"));
        const DWORD mutexError = GetLastError();
        CheckWin32(static_cast<bool>(singleton), L"Cannot create the single-instance lock.");
        if (mutexError == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(WindowClass, L"PaperShade");
            if (!existing) throw winrt::hresult_error(E_FAIL, L"PaperShade is still starting. Try again in a moment.");
            if (argc == 2 && std::wstring(raw[1]) == L"--startup") return 0;
            const UINT dispatched = command ? command : About;
            if (dispatched == About || dispatched == CustomTemperature) {
                CheckWin32(PostMessageW(existing, CommandMessage, dispatched, 0), L"Cannot open the PaperShade dialog.");
                return 0;
            }
            DWORD_PTR response = 0;
            CheckWin32(SendMessageTimeoutW(existing, CommandMessage, dispatched,
                0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &response) != 0,
                L"The running PaperShade instance is not responding.");
            return response ? 0 : 1;
        }
        if (command == Quit) return 0;
        if (command == ResetSettings) SaveSettings(Settings{});
        auto settings = LoadSettings();
        if (command == Pause) {
            settings.enabled = false;
            if (std::wstring(raw[1]) != L"--safe") SaveSettings(settings);
        }
        App app(settings);
        app.Initialize(instance);
        if (command && command != Pause && command != ResetSettings) app.Command(command);
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        CheckWin32(result != -1, L"The Windows message loop failed.");
        return static_cast<int>(message.wParam);
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"PaperShade could not start", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, winrt::to_hstring(error.what()).c_str(), L"PaperShade could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
}
#endif
