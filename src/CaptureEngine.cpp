#include "CaptureEngine.h"
#include "GpuRenderer.h"

#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <chrono>
#include <cwchar>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace paper {
namespace {

namespace capture = winrt::Windows::Graphics::Capture;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using Clock = std::chrono::steady_clock;
using Size = winrt::Windows::Graphics::SizeInt32;
using TimeSpan = winrt::Windows::Foundation::TimeSpan;
using AccessStatus = winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
constexpr auto FirstFrameTimeout = std::chrono::seconds(5);

void Check(HRESULT result, std::wstring_view message) {
    if (FAILED(result)) {
        throw winrt::hresult_error(result, winrt::hstring(message));
    }
}

void CheckWindow(BOOL succeeded, std::wstring_view message) {
    if (!succeeded) {
        const DWORD error = GetLastError();
        Check(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE), message);
    }
}

template <typename T>
void CloseForCleanup(T& object) noexcept {
    if (object) {
        // Cleanup must finish even after device loss or an already-closed capture.
        try {
            object.Close();
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW(L"PaperShade capture cleanup: ");
            OutputDebugStringW(error.message().c_str());
            OutputDebugStringW(L"\n");
        }
        object = nullptr;
    }
}

struct ScopedDpiAwareness {
    DPI_AWARENESS_CONTEXT previous =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ScopedDpiAwareness() {
        CheckWindow(previous != nullptr, L"Cannot obtain physical per-monitor display coordinates.");
    }
    ~ScopedDpiAwareness() {
        SetThreadDpiAwarenessContext(previous);
    }
};

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct Display {
    HMONITOR monitor = nullptr;
    RECT bounds{};
    std::wstring name;
    winrt::com_ptr<IDXGIAdapter1> adapter;
    winrt::com_ptr<IDXGIOutput6> output;
};

struct DisplayEnumeration {
    std::vector<Display> displays;
    std::exception_ptr failure;
};

BOOL CALLBACK CollectDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) noexcept {
    auto& result = *reinterpret_cast<DisplayEnumeration*>(parameter);
    try {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        CheckWindow(GetMonitorInfoW(monitor, &info), L"Cannot read an active monitor's geometry.");
        Display display;
        display.monitor = monitor;
        display.bounds = info.rcMonitor;
        display.name = info.szDevice;
        result.displays.push_back(std::move(display));
        return TRUE;
    } catch (const winrt::hresult_error&) {
        result.failure = std::current_exception();
        return FALSE;
    } catch (const std::exception&) {
        result.failure = std::current_exception();
        return FALSE;
    }
}

void RequireSdr(const DXGI_OUTPUT_DESC1& output) {
    if (output.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            winrt::hstring(std::wstring(output.DeviceName) +
                L": advanced display filters require an SDR Rec. 709 output. "
                L"HDR/advanced color is active or the output color space is unsupported "
                L"(DXGI color space " +
                std::to_wstring(static_cast<unsigned>(output.ColorSpace)) +
                L"). Turn off HDR for this display before enabling an advanced filter; "
                L"PaperShade will not display a clipped SDR capture."));
    }
}

std::vector<Display> EnumerateDisplays(IDXGIFactory2* factory) {
    DisplayEnumeration enumeration;
    const BOOL enumerated = EnumDisplayMonitors(
        nullptr, nullptr, CollectDisplay, reinterpret_cast<LPARAM>(&enumeration));
    if (enumeration.failure) {
        std::rethrow_exception(enumeration.failure);
    }
    CheckWindow(enumerated, L"Cannot enumerate the active desktop monitors.");
    if (enumeration.displays.empty()) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            L"No active desktop monitors are available for capture.");
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, adapter.put());
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        Check(adapterResult, L"Cannot enumerate the display's DXGI hardware adapters.");
        DXGI_ADAPTER_DESC1 adapterDescription{};
        Check(adapter->GetDesc1(&adapterDescription), L"Cannot identify a display adapter.");

        for (UINT outputIndex = 0;; ++outputIndex) {
            winrt::com_ptr<IDXGIOutput> output;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, output.put());
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            Check(outputResult, L"Cannot enumerate a display adapter's outputs.");
            DXGI_OUTPUT_DESC description{};
            Check(output->GetDesc(&description), L"Cannot read a DXGI display output.");
            if (!description.AttachedToDesktop) {
                continue;
            }
            auto advancedOutput = output.try_as<IDXGIOutput6>();
            if (!advancedOutput) {
                throw winrt::hresult_error(E_NOINTERFACE,
                    L"The display driver cannot report its HDR/SDR color space through "
                    L"IDXGIOutput6. Advanced filters require a supported Windows 11 driver.");
            }
            DXGI_OUTPUT_DESC1 advancedDescription{};
            Check(advancedOutput->GetDesc1(&advancedDescription),
                L"Cannot verify the display's HDR/SDR color space.");
            RequireSdr(advancedDescription);

            for (auto& display : enumeration.displays) {
                if (description.Monitor != display.monitor || display.adapter) {
                    continue;
                }
                if (adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    throw winrt::hresult_error(DXGI_ERROR_UNSUPPORTED,
                        L"A desktop monitor is hosted by a software adapter. Advanced filters "
                        L"require a hardware Direct3D 11 device; WARP fallback is not used.");
                }
                if (!EqualRect(&display.bounds, &advancedDescription.DesktopCoordinates) ||
                    advancedDescription.Monitor != display.monitor) {
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_RETRY),
                        L"The desktop layout changed during capture initialization. "
                        L"Enable the filter again after the display configuration has settled.");
                }
                display.adapter = adapter;
                display.output = advancedOutput;
            }
        }
    }
    for (const auto& display : enumeration.displays) {
        if (!display.adapter) {
            throw winrt::hresult_error(DXGI_ERROR_UNSUPPORTED,
                winrt::hstring(display.name +
                    L": no hardware DXGI adapter/output could be matched to this monitor. "
                    L"Remote, indirect, or unsupported display drivers cannot be captured "
                    L"by this advanced filter."));
        }
    }
    return std::move(enumeration.displays);
}

struct AdapterResources {
    LUID id{};
    std::wstring name;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    direct3d::IDirect3DDevice captureDevice{nullptr};
    winrt::com_ptr<IDCompositionDevice> composition;
    std::unique_ptr<GpuRenderer> renderer;

    explicit AdapterResources(IDXGIAdapter1* adapter) {
        DXGI_ADAPTER_DESC1 description{};
        Check(adapter->GetDesc1(&description), L"Cannot identify the monitor's GPU.");
        id = description.AdapterLuid;
        name = description.Description;
        constexpr D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL obtained{};
        Check(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, 1, D3D11_SDK_VERSION,
            device.put(), &obtained, context.put()),
            L"Cannot create a hardware Direct3D 11 feature-level 11_0 device on the "
            L"monitor's adapter. Update the display driver; no software fallback is used.");
        if (obtained < D3D_FEATURE_LEVEL_11_0) {
            throw winrt::hresult_error(DXGI_ERROR_UNSUPPORTED,
                L"The monitor's hardware adapter does not support Direct3D feature level 11_0.");
        }
        // WGC also uses this device internally; our immediate-context work stays on the owner thread.
        context.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
        auto dxgiDevice = device.as<IDXGIDevice1>();
        Check(dxgiDevice->SetMaximumFrameLatency(1), L"Cannot limit the GPU's queued frames.");
        winrt::com_ptr<::IInspectable> inspectable;
        Check(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()),
            L"Cannot expose the monitor's Direct3D device to Windows.Graphics.Capture.");
        captureDevice = inspectable.as<direct3d::IDirect3DDevice>();
        Check(DCompositionCreateDevice(dxgiDevice.get(), __uuidof(IDCompositionDevice),
            composition.put_void()), L"Cannot create the hardware DirectComposition device.");
        renderer = std::make_unique<GpuRenderer>(device.get());
    }

    ~AdapterResources() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        CloseForCleanup(captureDevice);
    }
};

struct NotificationState {
    struct Signal {
        bool frame = false;
        bool closed = false;
    };
    struct Lock {
        SRWLOCK& lock;
        explicit Lock(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
        ~Lock() { ReleaseSRWLockExclusive(&lock); }
    };

    SRWLOCK gate = SRWLOCK_INIT;
    HWND owner;
    std::vector<Signal> signals;
    bool active = true;
    bool messagePending = false;
    bool suppressFrames = false;
    DWORD postError = ERROR_SUCCESS;

    NotificationState(HWND window, std::size_t count) : owner(window), signals(count) {}

    void PostLocked() noexcept {
        if (messagePending) {
            return;
        }
        if (PostMessageW(owner, CaptureFrameMessage, 0, 0)) {
            messagePending = true;
        } else if (!postError) {
            postError = GetLastError();
            if (!postError) {
                postError = ERROR_NOT_ENOUGH_QUOTA;
            }
        }
    }

    void Notify(std::size_t index, bool closed) noexcept {
        Lock lock(gate);
        if (!active || index >= signals.size()) {
            return;
        }
        if (closed) {
            signals[index].closed = true;
        } else {
            signals[index].frame = true;
        }
        // Closure must wake the owner immediately, even while frames are timer-coalesced.
        if (closed || !suppressFrames) {
            PostLocked();
        }
    }

    void NotifyAccessComplete() noexcept {
        Lock lock(gate);
        if (active) PostLocked();
    }

    void Finish(bool suppress) noexcept {
        Lock lock(gate);
        suppressFrames = suppress;
        if (active && std::any_of(signals.begin(), signals.end(), [suppress](const Signal& signal) {
                return signal.closed || (!suppress && signal.frame);
            })) {
            PostLocked();
        }
    }

    void Deactivate() noexcept {
        Lock lock(gate);
        active = false;
        owner = nullptr;
    }
};

struct MonitorCapture {
    Display display;
    std::shared_ptr<AdapterResources> gpu;
    HWND overlay = nullptr;
    Size size{};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
    capture::GraphicsCaptureItem::Closed_revoker itemClosed;
    winrt::com_ptr<IDXGISwapChain1> swapChain;
    winrt::com_ptr<IDCompositionTarget> target;
    winrt::com_ptr<IDCompositionVisual> visual;
    winrt::com_ptr<ID3D11Texture2D> sampleTexture;
    winrt::com_ptr<ID3D11ShaderResourceView> sampleView;
    winrt::com_ptr<ID3D11RenderTargetView> renderTarget;
    bool pending = false;
    bool closed = false;
    bool receivedFrame = false;
    bool visible = false;
    Clock::time_point nextRender{};
    Clock::time_point firstFrameDeadline{};

    void Hide() noexcept {
        if (overlay) {
            ShowWindow(overlay, SW_HIDE);
            visible = false;
        }
    }

    ~MonitorCapture() {
        Hide();
        frameArrived.revoke();
        itemClosed.revoke();
        CloseForCleanup(session);
        CloseForCleanup(pool);
        item = nullptr;
        if (target) {
            target->SetRoot(nullptr);
        }
        if (visual) {
            visual->SetContent(nullptr);
        }
        target = nullptr;
        visual = nullptr;
        renderTarget = nullptr;
        sampleView = nullptr;
        sampleTexture = nullptr;
        swapChain = nullptr;
        if (overlay) {
            DestroyWindow(overlay);
        }
    }
};

struct ScopedFrame {
    capture::Direct3D11CaptureFrame frame{nullptr};
    ~ScopedFrame() { CloseForCleanup(frame); }

    void Close() {
        if (frame) {
            auto previous = std::exchange(frame, nullptr);
            previous.Close();
        }
    }
};

struct Failure {
    HRESULT code;
    std::wstring text;
};

Failure DescribeFailure(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const winrt::hresult_error& error) {
        wchar_t code[32]{};
        swprintf_s(code, L" (HRESULT 0x%08X)", static_cast<unsigned>(error.code().value));
        return {error.code(), std::wstring(error.message()) + code};
    } catch (const std::bad_alloc&) {
        return {E_OUTOFMEMORY, L"Not enough memory to run desktop capture."};
    } catch (const std::exception& error) {
        return {E_FAIL, std::wstring(winrt::to_hstring(error.what()))};
    }
}

}

struct CaptureEngine::Impl {
    ShaderConstants constants{};
    HWND owner = nullptr;
    HINSTANCE module = nullptr;
    std::wstring windowClass;
    bool classRegistered = false;
    bool running = false;
    bool awaitingAccess = false;
    winrt::Windows::Foundation::IAsyncOperation<AccessStatus> borderlessAccess{nullptr};
    bool timerArmed = false;
    Clock::time_point timerDeadline{};
    Clock::duration interval{};
    CaptureStats stats;
    std::wstring error;
    winrt::com_ptr<IDXGIFactory2> factory;
    std::vector<std::shared_ptr<AdapterResources>> adapters;
    std::vector<std::unique_ptr<MonitorCapture>> monitors;
    std::shared_ptr<NotificationState> notifications;

    void Stop() noexcept {
        running = false;
        // Hide every output before any potentially blocking WinRT revocation/Close operation.
        for (const auto& monitor : monitors) {
            monitor->Hide();
        }
        if (notifications) {
            notifications->Deactivate();
        }
        awaitingAccess = false;
        if (borderlessAccess) {
            try {
                borderlessAccess.Cancel();
            } catch (const winrt::hresult_error& detail) {
                OutputDebugStringW(L"PaperShade borderless-permission cleanup: ");
                OutputDebugStringW(detail.message().c_str());
                OutputDebugStringW(L"\n");
            }
            borderlessAccess = nullptr;
        }
        if (owner) {
            if (timerArmed) {
                KillTimer(owner, CaptureTimer);
            }
            // Stale posted messages are harmless behind the running/error guards.
            // Do not PeekMessage here: it can dispatch incoming commands during teardown.
        }
        timerArmed = false;
        monitors.clear();
        adapters.clear();
        factory = nullptr;
        notifications.reset();
        if (classRegistered) {
            UnregisterClassW(windowClass.c_str(), module);
        }
        classRegistered = false;
        windowClass.clear();
        module = nullptr;
        owner = nullptr;
        stats = {};
    }

    void ReportFailure(const std::exception_ptr& failure, bool asynchronous) {
        const HWND destination = owner;
        Stop();
        auto detail = DescribeFailure(failure);
        error = L"Advanced display capture failed: " + detail.text;
        if (!asynchronous || !destination ||
            !PostMessageW(destination, CaptureFailureMessage, 0, 0)) {
            throw winrt::hresult_error(detail.code, winrt::hstring(error));
        }
    }

    std::shared_ptr<AdapterResources> GetAdapter(IDXGIAdapter1* adapter) {
        DXGI_ADAPTER_DESC1 description{};
        Check(adapter->GetDesc1(&description), L"Cannot read the monitor's hardware adapter.");
        for (const auto& existing : adapters) {
            if (existing->id.HighPart == description.AdapterLuid.HighPart &&
                existing->id.LowPart == description.AdapterLuid.LowPart) {
                return existing;
            }
        }
        auto resources = std::make_shared<AdapterResources>(adapter);
        if (!stats.adapter.empty()) {
            stats.adapter += L"; ";
        }
        stats.adapter += resources->name;
        adapters.push_back(resources);
        return resources;
    }

    void CreateOverlay(MonitorCapture& monitor) {
        const auto& bounds = monitor.display.bounds;
        // Layered + TRANSPARENT is cross-process click-through; HTTRANSPARENT alone is not.
        constexpr DWORD style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
        monitor.overlay = CreateWindowExW(style, windowClass.c_str(), L"PaperShade display filter",
            WS_POPUP, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, nullptr, nullptr, module, nullptr);
        CheckWindow(monitor.overlay != nullptr, L"Cannot create a nonactivating display overlay.");
        CheckWindow(SetLayeredWindowAttributes(monitor.overlay, 0, 255, LWA_ALPHA),
            L"Cannot configure the display overlay for fully opaque, click-through composition.");
        CheckWindow(SetWindowDisplayAffinity(monitor.overlay, WDA_EXCLUDEFROMCAPTURE),
            L"Windows refused WDA_EXCLUDEFROMCAPTURE for the display overlay. "
            L"Capture has not started because an unexcluded overlay would cause feedback.");
        DWORD affinity = WDA_NONE;
        CheckWindow(GetWindowDisplayAffinity(monitor.overlay, &affinity),
            L"Cannot verify the display overlay's capture exclusion.");
        if (affinity != WDA_EXCLUDEFROMCAPTURE) {
            throw winrt::hresult_error(E_ACCESSDENIED,
                L"Windows did not retain WDA_EXCLUDEFROMCAPTURE. Capture is refused to avoid feedback.");
        }

        DXGI_SWAP_CHAIN_DESC1 swap{};
        swap.Width = static_cast<UINT>(monitor.size.Width);
        swap.Height = static_cast<UINT>(monitor.size.Height);
        swap.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2;
        swap.Scaling = DXGI_SCALING_STRETCH;
        swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swap.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        Check(factory->CreateSwapChainForComposition(
            monitor.gpu->device.get(), &swap, nullptr, monitor.swapChain.put()),
            L"Cannot create the hardware composition swap chain.");
        auto& composition = monitor.gpu->composition;
        Check(composition->CreateTargetForHwnd(monitor.overlay, TRUE, monitor.target.put()),
            L"Cannot bind DirectComposition to the display overlay.");
        Check(composition->CreateVisual(monitor.visual.put()), L"Cannot create the display visual.");
        Check(monitor.visual->SetContent(monitor.swapChain.get()),
            L"Cannot attach the display's swap chain to its composition visual.");
        Check(monitor.target->SetRoot(monitor.visual.get()), L"Cannot attach the display visual.");
        Check(composition->Commit(), L"Cannot commit the display composition tree.");

        winrt::com_ptr<ID3D11Texture2D> backBuffer;
        Check(monitor.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()),
            L"Cannot obtain the display swap chain's GPU back buffer.");
        Check(monitor.gpu->device->CreateRenderTargetView(
            backBuffer.get(), nullptr, monitor.renderTarget.put()),
            L"Cannot create the display's GPU render target.");
        D3D11_TEXTURE2D_DESC sample{};
        sample.Width = swap.Width;
        sample.Height = swap.Height;
        sample.MipLevels = 1;
        sample.ArraySize = 1;
        sample.Format = swap.Format;
        sample.SampleDesc.Count = 1;
        sample.Usage = D3D11_USAGE_DEFAULT;
        sample.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // Capture-pool surfaces can rotate and need not be SRV-bindable. Keep one GPU-only
        // sampling texture/view instead of creating views or textures for each arriving frame.
        Check(monitor.gpu->device->CreateTexture2D(&sample, nullptr, monitor.sampleTexture.put()),
            L"Cannot allocate the persistent GPU capture sampling texture.");
        Check(monitor.gpu->device->CreateShaderResourceView(
            monitor.sampleTexture.get(), nullptr, monitor.sampleView.put()),
            L"Cannot create the persistent capture sampling view.");
    }

    void Start(HWND window, const Settings& settings) {
        Stop();
        error.clear();
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!window || thread != GetCurrentThreadId() || process != GetCurrentProcessId()) {
            throw winrt::hresult_error(E_INVALIDARG,
                L"Capture must start on the message-loop thread of a valid owner window.");
        }
        if (!ValidPreset(static_cast<std::uint32_t>(settings.preset)) || !ValidFps(settings.fps) ||
            !ValidKelvin(settings.temperatureKelvin) ||
            settings.pixelSize < 1 || settings.pixelSize > 4) {
            throw winrt::hresult_error(E_INVALIDARG,
                L"Capture settings require a valid preset, 10/15/30/60 fps, 1000-6500 K, and a pixel size from 1 to 4.");
        }
        owner = window;
        ScopedDpiAwareness dpi;
        BOOL composed = FALSE;
        Check(DwmIsCompositionEnabled(&composed), L"Cannot query desktop composition support.");
        if (!composed || !capture::GraphicsCaptureSession::IsSupported()) {
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                L"Windows.Graphics.Capture and desktop composition are required. "
                L"Use a supported Windows 11 desktop and hardware display driver.");
        }
        auto poolFactory = winrt::get_activation_factory<
            capture::Direct3D11CaptureFramePool>().try_as<capture::IDirect3D11CaptureFramePoolStatics2>();
        if (!poolFactory) {
            throw winrt::hresult_error(E_NOINTERFACE,
                L"This Windows installation does not support free-threaded desktop capture.");
        }
        auto itemFactory = winrt::get_activation_factory<
            capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        Check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), factory.put_void()),
            L"Cannot create the DXGI factory required for hardware monitor capture.");
        auto displays = EnumerateDisplays(factory.get());
        module = GetModuleHandleW(nullptr);
        windowClass = L"PaperShade.CaptureOverlay." +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSEXW definition{};
        definition.cbSize = sizeof(definition);
        definition.lpfnWndProc = OverlayProc;
        definition.hInstance = module;
        definition.lpszClassName = windowClass.c_str();
        CheckWindow(RegisterClassExW(&definition) != 0, L"Cannot register the display overlay class.");
        classRegistered = true;
        const TimeSpan requestedInterval{(10'000'000LL + settings.fps - 1) / settings.fps};
        interval = std::chrono::duration_cast<Clock::duration>(requestedInterval);
        constants = Constants(settings.preset, settings.pixelSize, settings.temperatureKelvin);
        monitors.reserve(displays.size());

        // Finish and verify ALL hidden, excluded overlays before starting ANY capture session.
        for (auto& display : displays) {
            auto monitor = std::make_unique<MonitorCapture>();
            monitor->display = std::move(display);
            monitors.push_back(std::move(monitor));
            auto& current = *monitors.back();
            try {
                current.gpu = GetAdapter(current.display.adapter.get());
                Check(itemFactory->CreateForMonitor(current.display.monitor,
                    winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(current.item)),
                    L"Windows refused monitor capture. Check capture permissions, policy, "
                    L"and Windows.Graphics.Capture support; no permission bypass is attempted.");
                current.size = current.item.Size();
                const auto& bounds = current.display.bounds;
                if (current.size.Width <= 0 || current.size.Height <= 0 ||
                    current.size.Width != bounds.right - bounds.left ||
                    current.size.Height != bounds.bottom - bounds.top) {
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_RETRY),
                        L"The capture dimensions do not match the physical monitor. "
                        L"The display layout may have changed; restart the filter.");
                }
                CreateOverlay(current);
            } catch (const winrt::hresult_error& detail) {
                throw winrt::hresult_error(detail.code(),
                    winrt::hstring(current.display.name + L": " + std::wstring(detail.message())));
            }
        }

        notifications = std::make_shared<NotificationState>(owner, monitors.size());
        const std::weak_ptr<NotificationState> weak = notifications;
        stats.monitors = static_cast<std::uint32_t>(monitors.size());
        stats.sourceRateLimited = true;
        for (std::size_t index = 0; index < monitors.size(); ++index) {
            auto& monitor = *monitors[index];
            monitor.pool = poolFactory.CreateFreeThreaded(monitor.gpu->captureDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2, monitor.size);
            monitor.frameArrived = monitor.pool.FrameArrived(winrt::auto_revoke,
                [weak, index](const auto&, const auto&) noexcept {
                    if (auto state = weak.lock()) {
                        state->Notify(index, false);
                    }
                });
            monitor.itemClosed = monitor.item.Closed(winrt::auto_revoke,
                [weak, index](const auto&, const auto&) noexcept {
                    if (auto state = weak.lock()) {
                        state->Notify(index, true);
                    }
                });
            monitor.session = monitor.pool.CreateCaptureSession(monitor.item);
            auto cursorControl = monitor.session.try_as<capture::IGraphicsCaptureSession2>();
            if (!cursorControl) {
                throw winrt::hresult_error(E_NOINTERFACE,
                    L"This capture session cannot exclude the hardware cursor. "
                    L"A supported Windows 11 capture implementation is required.");
            }
            cursorControl.IsCursorCaptureEnabled(false);
            if (auto limiter = monitor.session.try_as<capture::IGraphicsCaptureSession5>()) {
                limiter.MinUpdateInterval(requestedInterval);
            } else {
                stats.sourceRateLimited = false;
            }
        }
        running = true;
        if (!settings.hideCaptureIndicator) {
            BeginSessions(CaptureBorderState::VisibleByChoice);
            return;
        }
        awaitingAccess = true;
        borderlessAccess = capture::GraphicsCaptureAccess::RequestAccessAsync(
            capture::GraphicsCaptureAccessKind::Borderless);
        // Never block the STA/message loop on Windows consent. Completion only wakes the owner;
        // the weak notification state makes pause, exit, and preset changes safe while it is pending.
        borderlessAccess.Completed([weak](const auto&, auto) noexcept {
            if (auto state = weak.lock()) state->NotifyAccessComplete();
        });
    }

    void BeginSessions(CaptureBorderState requested) {
        bool borderless = requested == CaptureBorderState::Borderless;
        if (borderless) {
            for (const auto& monitor : monitors) {
                if (!monitor->session.try_as<capture::IGraphicsCaptureSession3>()) {
                    borderless = false;
                    requested = CaptureBorderState::RequiredByWindows;
                    break;
                }
            }
        }
        for (const auto& monitor : monitors) {
            if (auto border = monitor->session.try_as<capture::IGraphicsCaptureSession3>()) {
                border.IsBorderRequired(!borderless);
                if (border.IsBorderRequired() != !borderless) {
                    throw winrt::hresult_error(E_FAIL, L"Windows did not retain the requested capture-border setting.");
                }
            }
        }
        stats.border = requested;
        for (const auto& monitor : monitors) {
            monitor->session.StartCapture();
            monitor->firstFrameDeadline = Clock::now() + FirstFrameTimeout;
        }
        Schedule();
        notifications->Finish(false);
        CheckWindow(PostMessageW(owner, CaptureBorderMessage, 0, 0),
            L"Cannot report the Windows capture-indicator status.");
    }

    void CollectSignals(bool fromTimer) {
        DWORD postError = ERROR_SUCCESS;
        {
            NotificationState::Lock lock(notifications->gate);
            if (!fromTimer) {
                notifications->messagePending = false;
            }
            postError = notifications->postError;
            for (std::size_t index = 0; index < monitors.size(); ++index) {
                auto& signal = notifications->signals[index];
                monitors[index]->pending |= signal.frame;
                monitors[index]->closed |= signal.closed;
                signal = {};
            }
        }
        if (postError) {
            Check(HRESULT_FROM_WIN32(postError),
                L"A capture notification could not reach the owner window.");
        }
    }

    void Render(MonitorCapture& monitor) {
        DXGI_OUTPUT_DESC1 output{};
        Check(monitor.display.output->GetDesc1(&output), L"Cannot validate the captured output.");
        RequireSdr(output);
        if (!output.AttachedToDesktop || output.Monitor != monitor.display.monitor ||
            !EqualRect(&output.DesktopCoordinates, &monitor.display.bounds)) {
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_RETRY),
                L"The captured monitor was disconnected or its layout changed. Restart the filter.");
        }
        BOOL validComposition = FALSE;
        Check(monitor.gpu->composition->CheckDeviceState(&validComposition),
            L"Cannot validate the display composition device.");
        if (!validComposition) {
            throw winrt::hresult_error(DXGI_ERROR_DEVICE_REMOVED,
                L"The display composition device was lost. Restart the filter.");
        }
        Check(monitor.gpu->device->GetDeviceRemovedReason(), L"The monitor's hardware GPU device was lost.");

        ScopedFrame latest;
        // The pool has only two buffers. Drain at most those two, never spin chasing a producer.
        for (unsigned count = 0; count < 2; ++count) {
            auto next = monitor.pool.TryGetNextFrame();
            if (!next) {
                break;
            }
            latest.Close();
            latest.frame = std::move(next);
            const Size content = latest.frame.ContentSize();
            if (content.Width != monitor.size.Width || content.Height != monitor.size.Height) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_RETRY),
                    L"The captured monitor changed size or became unavailable. "
                    L"The overlay has been stopped instead of stretching or freezing an old frame.");
            }
        }
        if (!latest.frame) {
            return;
        }
        auto surface = latest.frame.Surface();
        if (!surface) {
            throw winrt::hresult_error(E_UNEXPECTED, L"Windows returned a capture frame without a GPU surface.");
        }
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        Check(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()),
            L"Cannot access the captured frame's Direct3D texture.");
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        const UINT width = static_cast<UINT>(monitor.size.Width);
        const UINT height = static_cast<UINT>(monitor.size.Height);
        if (description.Width < width || description.Height < height ||
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            description.ArraySize != 1 || description.SampleDesc.Count != 1) {
            throw winrt::hresult_error(DXGI_ERROR_UNSUPPORTED,
                L"The capture driver returned an incompatible SDR GPU texture.");
        }
        const D3D11_BOX region{0, 0, 0, width, height, 1};
        monitor.gpu->context->CopySubresourceRegion(
            monitor.sampleTexture.get(), 0, 0, 0, 0, source.get(), 0, &region);
        monitor.gpu->renderer->Draw(monitor.gpu->context.get(), monitor.sampleView.get(),
            monitor.renderTarget.get(), width, height, constants);
        // Vsync, rather than a polling loop or tearing, bounds the composition submission queue.
        Check(monitor.swapChain->Present(1, 0), L"Cannot present the filtered display frame.");
        latest.Close();
        monitor.nextRender = Clock::now() + interval;
        monitor.receivedFrame = true;
        if (!monitor.visible) {
            CheckWindow(SetWindowPos(monitor.overlay, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW),
                L"Cannot show the filtered display without activating it.");
            monitor.visible = true;
        }
        ++stats.frames;
    }

    void Schedule() {
        std::optional<Clock::time_point> deadline;
        bool deferredFrames = false;
        const auto takeEarlier = [&deadline](Clock::time_point candidate) {
            if (!deadline || candidate < *deadline) {
                deadline = candidate;
            }
        };
        for (const auto& monitor : monitors) {
            if (!monitor->receivedFrame) {
                takeEarlier(monitor->firstFrameDeadline);
            }
            if (monitor->pending) {
                deferredFrames = true;
                takeEarlier(monitor->nextRender);
            }
        }
        if (!deadline) {
            if (timerArmed) {
                KillTimer(owner, CaptureTimer);
                timerArmed = false;
            }
        } else if (!timerArmed || timerDeadline != *deadline) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
            const auto delay = static_cast<UINT>(std::clamp<std::int64_t>(
                remaining.count(), 1, USER_TIMER_MAXIMUM));
            CheckWindow(SetCoalescableTimer(owner, CaptureTimer, delay, nullptr, 1) != 0,
                L"Cannot schedule the capture frame deadline.");
            timerArmed = true;
            timerDeadline = *deadline;
        }
        // A startup-only watchdog must not suppress the very first frame notification.
        notifications->Finish(deferredFrames);
    }

    void Process(bool fromTimer) {
        CollectSignals(fromTimer);
        for (const auto& monitor : monitors) {
            if (monitor->closed) {
                throw winrt::hresult_error(RO_E_CLOSED,
                    winrt::hstring(monitor->display.name +
                        L": Windows closed the monitor capture. The filter has been stopped."));
            }
        }
        if (awaitingAccess) {
            if (borderlessAccess.Status() == winrt::Windows::Foundation::AsyncStatus::Started) return;
            const auto access = borderlessAccess.GetResults();
            borderlessAccess = nullptr;
            awaitingAccess = false;
            BeginSessions(access == AccessStatus::Allowed
                ? CaptureBorderState::Borderless : CaptureBorderState::RequiredByWindows);
        }
        for (const auto& monitor : monitors) {
            if (monitor->pending && Clock::now() >= monitor->nextRender) {
                monitor->pending = false;
                try {
                    Render(*monitor);
                } catch (const winrt::hresult_error& detail) {
                    throw winrt::hresult_error(detail.code(),
                        winrt::hstring(monitor->display.name + L": " + std::wstring(detail.message())));
                }
            }
            if (!monitor->receivedFrame && Clock::now() >= monitor->firstFrameDeadline) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                    winrt::hstring(monitor->display.name +
                        L": Windows did not deliver an initial desktop capture frame within five seconds. "
                        L"Check capture permissions, desktop availability, and the display driver."));
            }
        }
        Schedule();
    }
};

CaptureEngine::CaptureEngine() : impl_(std::make_unique<Impl>()) {}

CaptureEngine::~CaptureEngine() {
    Stop();
}

void CaptureEngine::Start(HWND owner, const Settings& settings) {
    try {
        impl_->Start(owner, settings);
    } catch (const winrt::hresult_error&) {
        impl_->ReportFailure(std::current_exception(), false);
    } catch (const std::exception&) {
        impl_->ReportFailure(std::current_exception(), false);
    }
}

void CaptureEngine::Stop() noexcept {
    impl_->Stop();
}

void CaptureEngine::HandleFrame() {
    if (!impl_->running) {
        if (impl_->notifications) {
            NotificationState::Lock lock(impl_->notifications->gate);
            impl_->notifications->messagePending = false;
        }
        return;
    }
    try {
        impl_->Process(false);
    } catch (const winrt::hresult_error&) {
        impl_->ReportFailure(std::current_exception(), true);
    } catch (const std::exception&) {
        impl_->ReportFailure(std::current_exception(), true);
    }
}

void CaptureEngine::HandleTimer() {
    if (!impl_->running || !impl_->timerArmed) {
        return;
    }
    KillTimer(impl_->owner, CaptureTimer);
    impl_->timerArmed = false;
    try {
        impl_->Process(true);
    } catch (const winrt::hresult_error&) {
        impl_->ReportFailure(std::current_exception(), true);
    } catch (const std::exception&) {
        impl_->ReportFailure(std::current_exception(), true);
    }
}

CaptureStats CaptureEngine::Stats() const {
    return impl_->stats;
}

std::wstring CaptureEngine::LastError() const {
    return impl_->error;
}

}
