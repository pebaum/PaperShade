#define PAPERSHADE_APP_TESTS
#include "../src/Main.cpp"

#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void PumpFor(DWORD milliseconds) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message != WM_QUIT) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        const auto now = GetTickCount64();
        if (now >= until) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::min<ULONGLONG>(50, until - now)),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (true);
}

void TestPolicy() {
    paper::DisplayRecovery policy;
    policy.DisplayChanged(100);
    policy.DisplayChanged(200);
    Require(!policy.Begin(899) && policy.Begin(900), "Topology events were not debounced from the last change.");
    Require(policy.Attempts() == 1, "Recovery attempt was not counted.");
    Require(policy.Retry(1000), "First retry rejected.");
    Require(!policy.Begin(2399) && policy.Begin(2400), "Recovery backoff was not honored.");
    Require(policy.Retry(2500) && policy.Begin(4600), "Third recovery attempt was not scheduled.");
    Require(!policy.Retry(5000) && !policy.Pending() && policy.Exhausted(), "Recovery retries are not bounded.");
    policy.DisplayChanged(6000);
    policy.Cancel();
    Require(!policy.Begin(10000) && !policy.Exhausted(), "Pause did not cancel a pending restart.");
    policy.DisplayChanged(11000);
    Require(policy.Begin(11700), "New topology could not recover after cancellation.");
    policy.Complete();
    Require(policy.Attempts() == 0 && !policy.Pending(), "Successful frames did not clear recovery state.");

    Require(paper::IsRecoverableDisplayError(DXGI_ERROR_DEVICE_REMOVED), "Device loss was not recoverable.");
    Require(paper::IsRecoverableDisplayError(HRESULT_FROM_WIN32(ERROR_RETRY)), "Changed geometry was not recoverable.");
    Require(paper::IsRecoverableDisplayError(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)), "Temporarily missing displays were not recoverable.");
    Require(!paper::IsRecoverableDisplayError(E_ACCESSDENIED), "Permission denial must not cause automatic retries.");
    Require(!paper::IsRecoverableDisplayError(RO_E_CLOSED), "User-closed capture must not automatically restart.");
    Require(!paper::IsRecoverableDisplayError(DXGI_ERROR_UNSUPPORTED), "Unsupported displays must surface an error.");
}

void SendTopologyBurst(HWND window) {
    for (unsigned index = 0; index < 20; ++index) {
        SendMessageW(window, WM_DISPLAYCHANGE, 32, MAKELPARAM(2560, 1440));
        SendMessageW(window, paper::CaptureTopologyMessage, 0, 0);
    }
}

}

int main(int argc, char** argv) {
    using namespace paper;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        CheckWin32(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            L"Cannot establish physical-pixel DPI awareness.");
        TestPolicy();
        const bool live = argc == 2 && std::string(argv[1]) == "--live";
        Settings settings;
        settings.preset = Preset::Original;
        App app(settings);
        app.InitializeForTest(GetModuleHandleW(nullptr));
        const HWND window = app.Window();
        app.Command(Enable);

        const auto entries = app.TestEngineEntries();
        app.TestCriticalSection([&] {
            SendTopologyBurst(window);
            SendMessageW(window, WM_TIMER, RestartTimer, 0);
            SendMessageW(window, CaptureFrameMessage, 0, 0);
            SendMessageW(window, DeferredWorkMessage, 0, 0);
            SendMessageW(window, CommandMessage, Pause, 0);
            Require(app.TestEngineEntries() == entries + 1,
                "A display notification recursively entered capture teardown.");
        });
        PumpFor(1000);
        Require(!app.TestRecoveryPending(), "Pause left a pending recovery.");
        Require(SendMessageW(window, QueryMessage, 0, 0) == 0, "A deferred restart undid the user's pause.");
        Require(app.TestErrors() == 0, "Deferring reentrant messages caused an application error.");

        app.Command(Enable);
        SendTopologyBurst(window);
        PumpFor(100);
        Require(app.TestRecoveryPending(), "Recovery restarted before the topology settled.");
        PumpFor(900);
        Require(!app.TestRecoveryPending() && app.TestErrors() == 0, "A stable topology did not finish recovery.");

        if (live) {
            app.Command(PresetBase + static_cast<UINT>(Preset::Ink4));
            for (unsigned count = 0; count < 100 && app.TestCaptureStats().frames == 0; ++count) PumpFor(50);
            Require(app.TestCaptureStats().frames > 0, "Live capture did not start.");
            const auto displays = app.TestCaptureStats().monitors;
            Require(displays > 0, "No active displays were captured.");
            for (unsigned cycle = 0; cycle < 3; ++cycle) {
                const auto before = app.TestEngineEntries();
                app.TestCriticalSection([&] {
                    SendTopologyBurst(window);
                    SendMessageW(window, WM_TIMER, CaptureTimer, 0);
                    Require(app.TestEngineEntries() == before + 1,
                        "Live capture reentered an active graphics operation.");
                });
                PumpFor(1200);
                for (unsigned count = 0; count < 100 && app.TestCaptureStats().frames == 0; ++count) PumpFor(50);
                Require(app.TestErrors() == 0 && app.TestCaptureStats().frames > 0,
                    "Live capture did not resume after the monitor-change storm.");
                Require(app.TestCaptureStats().monitors == displays, "Recovery lost a connected display.");
            }
            app.Command(Pause);
            PumpFor(100);
            Require(app.TestCaptureStats().monitors == 0, "Pause did not release the recovered capture.");
            std::cout << "Live recovery passed on " << displays << " connected displays.\n";
        }
        app.TestCriticalSection([&] {
            SendTopologyBurst(window);
            SendMessageW(window, CommandMessage, Quit, 0);
        });
        PumpFor(100);
        Require(SendMessageW(window, QueryMessage, 0, 0) == 0 && !app.TestRecoveryPending(),
            "A pending monitor restart survived Quit.");
        std::cout << "Reentrant display messages, debouncing, bounded retries, and pause cancellation passed.\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
