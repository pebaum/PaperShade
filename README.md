# PaperShade

A native tray/menu-bar app for whole-desktop grayscale, e-ink-style palettes,
and PlayStation 1 dithering on **Windows 11 and macOS**. No Electron, browser
runtime, Python runtime, subscription, saved screen recordings, network access,
or telemetry.

| Platform | CPU architectures | Native implementation |
| --- | --- | --- |
| Windows 11 | ARM64 (Snapdragon/ARM PCs), x64 (Intel/AMD PCs) | Win32, Magnification, Windows Graphics Capture, D3D11 |
| macOS 13 or newer | Intel x86_64 and Apple silicon ARM64 (M-series) | AppKit, ScreenCaptureKit, Metal |

The macOS **universal** application contains both Intel and Apple silicon code.
ARM32, 32-bit x86 Windows, Linux desktop filtering, and App Store certification
are not supported targets.

## Run on Windows

Use **PaperShade.exe from the ARM64 package on Snapdragon/ARM PCs** and the
**x64 package on Intel/AMD PCs**. These are the two native Windows 11 CPU
architectures; an emulated build is deliberately rejected because Windows'
Magnification API does not support WOW64.

The first launch is **paused**, and automatic startup is **off**. Find the paper
icon next to the clock; Windows may initially put it under the tray's up-arrow.
Left-click to toggle, or right-click to select a **Display style**. Selecting a
style enables it. There is no normal taskbar window.

**Ctrl+Alt+G** toggles the selected filter. **Ctrl+Alt+Shift+G** always pauses.
**Exit and restore colors** closes the app and restores the previous color
transform. Preferences are stored under `HKCU\Software\PaperShade`. The last
enabled state is remembered for subsequent launches.

**Start with Windows** is opt-in and creates a single per-user Run entry pointing
to the current executable. Keep the executable in a stable location before
enabling this. Disable the option before moving or deleting a portable copy.

**Hide screen-capture indicator** is enabled by default for advanced styles.
PaperShade requests Windows' borderless-capture permission asynchronously,
then turns off its own capture border when Windows allows it. If permission
or system policy requires a border, filtering continues with the border and
a notification explains why. You can turn this preference off in the tray menu.
No system-wide privacy setting is changed.

For a per-user installation and Start menu shortcut, run `Install.ps1` from the
extracted package (or `powershell -NoProfile -ExecutionPolicy Bypass -File
.\Install.ps1`). It copies the app to `%LOCALAPPDATA%\Programs\PaperShade`; it
does not require administrator access or enable automatic startup. To remove
the app, first disable startup and exit, then delete that installation folder
and the PaperShade Start menu shortcut.

## Run on macOS

Extract the macOS universal package and copy **PaperShade.app** to
**Applications** before granting permissions. Launch it and use the half-shaded circle
in the menu bar; it does not add a normal Dock window. First launch is paused.
Enable a style and grant **Screen Recording** access to PaperShade in
**System Settings > Privacy & Security** when macOS requests it. Some macOS
versions require reopening the app after a new permission grant.

**Control+Option+G** toggles filtering and **Control+Option+Shift+G** always
pauses. Mode, frame cap, and pattern-size preferences are saved separately from
the Windows installation.

Unlike Windows' six matrix-only modes, **all macOS styles use ScreenCaptureKit
and Metal**. There is no supported public macOS equivalent of the Windows
full-desktop color-matrix API used here. macOS owns its screen-recording privacy
indicator; PaperShade does not try to hide it or alter system privacy settings.
Screen capture is local and frames are never saved.

The build script ad-hoc signs the application, but this is **not Developer ID
signing or Apple notarization**. A downloaded build may need approval through
macOS' normal **Privacy & Security > Open Anyway** flow. Don't disable
Gatekeeper. A notarized public distribution requires an Apple Developer ID
signing identity and notarization credentials, neither of which belongs in this
repository. See [macOS-specific documentation](macOS/README.md).

## Styles

The same preset parameters, reference math, and dither matrices are shared by
both platforms through `core/include/PaperShadeCore.h`. The engine column below
describes Windows; macOS uses Metal for every style.

| Style | Behavior | Windows engine |
| --- | --- | --- |
| Natural | Rec. 709 weighted grayscale | Windows compositor matrix |
| Classic | Rec. 601 weighted grayscale | Windows compositor matrix |
| Equal-channel | Arithmetic mean of red, green, and blue | Windows compositor matrix |
| Soft paper | Reduced-contrast grayscale with softer black and white | Windows compositor matrix |
| High contrast | Increased contrast, clipped to the display range | Windows compositor matrix |
| Inverted | Light text / dark backgrounds, with grayscale inversion | Windows compositor matrix |
| E-ink crisp | Two shades, hard threshold | GPU |
| E-ink dithered | Two shades with a stable 4x4 ordered pattern | GPU |
| E-ink 4 shades | Four equally spaced shades with ordered dithering | GPU |
| E-ink 16 shades | Sixteen equally spaced shades with ordered dithering | GPU |
| PS1 grayscale | Grayscale conversion followed by original PS1 RGB555 dithering | GPU |
| PS1 original color | Original desktop color followed by PS1 RGB555 dithering | GPU |

Grayscale weights operate on the display's encoded RGB values, matching the
compositor's color-matrix model; they are not a linear-light photometric
conversion. E-ink styles simulate limited palettes, not physical panel
waveforms, ghosting, or flashing refresh cycles.

## Power and hardware

On Windows, the six basic grayscale styles set a compositor matrix and then
sleep in the message loop. They do not capture the screen, upload frames, initialize
D3D, or run an application redraw timer. A small recovery helper waits on a
process handle while a matrix filter is active.

E-ink and PS1 styles require real GPU work: Windows Graphics Capture, a
click-through overlay on each monitor, and one D3D11 shader pass. The default cap
is **15 fps**, with **10 / 15 / 30 / 60 fps** choices. Rendering is driven by
capture events rather than a fixed repaint loop. Windows 11 versions exposing
`MinUpdateInterval` also cap capture at the source; older versions still have a
render cap but may incur higher capture overhead. The cursor stays live
independently of the cap.

No software-rendering fallback is silently enabled: advanced modes require a
hardware Direct3D 11 feature-level 11.0 adapter. They use Windows' standard APIs,
not NVIDIA/AMD/Intel/Qualcomm-specific extensions. GPU load, memory, latency,
and battery cost depend on resolution, monitor count, drivers, frame cap, and
desktop activity. **Zero CPU/GPU cost and compatibility with every possible
GPU cannot honestly be promised.** Basic grayscale is the lowest-power choice.

Capture surfaces and swapchains need GPU memory proportional to the number of
display pixels. Capture resources are released on pause, exit, lock, suspend,
and screen-off, and rebuilt after display topology changes. No frames are
copied back to the CPU or saved to disk during normal operation.

## PS1 accuracy

At **1 pixel**, the PS1 styles use this original signed screen-space matrix:

```text
-4   0  -3   1
 2  -2   3  -1
-3   1  -4   0
 3  -1   2  -2
```

For each encoded 8-bit RGB channel, the shader adds the corresponding signed
entry, clamps to 0..255, then discards the lowest three bits to produce RGB555.
For display on a modern 8-bit desktop, a five-bit value is expanded by
`(value << 3) | (value >> 2)`. The matrix is anchored to each monitor's physical
pixel origin, without bilinear filtering or temporal noise.

The grayscale variant performs grayscale conversion first; the original-color
variant preserves the incoming RGB channels until quantization. **2 / 3 / 4
pixel** pattern sizes are intentionally artistic enlargements, not exact
native-pixel PS1 output.

This reproduces the PS1's dither/quantization stage for those input bytes. It
does not claim to reproduce primitive-specific dither enabling, textured
polygon interpolation, blending, original game resolution, CRT response,
or the console's complete rendering pipeline.

Technical references: [PSX-SPX's GPU dithering description](https://github.com/psx-spx/psx-spx.github.io/blob/b791ca25b7833b356b28917383f231078762a75d/docs/graphicsprocessingunitgpu.md#L1443-L1457)
and [DuckStation's software dither lookup](https://github.com/stenzek/duckstation/blob/0cb515fe449b8f0215177a77af3255df59a0e141/src/core/gpu_sw_rasterizer.cpp#L18-L31).
The exact five-bit codes are distinct from their modern display expansion:
bit replication can differ by one eight-bit code value from rounded `q5 * 255 / 31`
and is not a claim about the console's analog output.

## Windows limitations and safety

- Advanced filters currently target **SDR**. An active HDR output causes a
  clear error instead of silently clipping HDR content. Disable **Use HDR** in
  Windows Display settings for those modes, or use a compositor grayscale style.
- Secure desktops (UAC/sign-in), protected video, capture-excluded windows,
  some elevated/system surfaces, and exclusive-fullscreen applications cannot
  be guaranteed. Use borderless/windowed mode for games.
- Advanced-mode windows are explicitly excluded from capture to prevent
  self-capture feedback. Screenshots or sharing tools may therefore see the
  original desktop rather than the filtered appearance.
- Advanced modes request borderless capture through the documented
  `GraphicsCaptureAccess.RequestAccessAsync(Borderless)` and
  `GraphicsCaptureSession.IsBorderRequired(false)` APIs. The permission result
  is honored; the app does not bypass Windows policy. A different application
  capturing the same display can still require a border even when PaperShade
  does not. **About / active engine** shows PaperShade's border-permission state.
- The hardware cursor is intentionally not captured or recolored. Its motion
  is not held back by a low refresh cap.
- Night light, Windows color filters, other magnifiers, calibration, and
  subsequent display transforms can change the final appearance. Avoid
  simultaneously controlling the same compositor color effect with two apps.
- Pause/exit restore the pre-existing color transform. A waiting helper also
  attempts restoration after abnormal termination. It does not overwrite a
  different transform subsequently installed by another app.
- Capture errors remove the overlays and pause the app instead of leaving a
  frozen picture over the desktop. The emergency hotkey is registered before
  any effect is enabled.

## Build on Windows

Requires CMake 3.25 or newer, Visual Studio 2022's C++ desktop tools, and Windows
SDK 10.0.26100.0 or newer. Install the native/cross C++ tools for the target
architecture. There are no downloaded library dependencies. The release binary statically links the
MSVC runtime; operating-system graphics DLLs remain system dependencies.

From this folder:

```powershell
cmake --preset arm64
cmake --build --preset arm64
ctest --preset arm64

cmake --preset x64
cmake --build --preset x64
ctest --preset x64
```

The x64 math/shader tests can run under x64 emulation on an ARM64 machine.
The **app itself and compositor tests must use the native architecture**.
Shader tests compile the same HLSL used by the app and compare actual GPU output
against the CPU reference for every preset and pattern size.

`ShaderTests.exe --warp` explicitly exercises Microsoft's reference software
renderer for testing only; the app does not use that fallback.
`DesktopTests.exe --exercise` is a separate, opt-in live desktop exercise:
it briefly enables grayscale and advanced filters, checks restoration,
simulates a failed owner process, checks frame caps and input transparency,
then restores the original colors. It is deliberately not run by CTest.
`DesktopTests.exe --capture-only` targets borderless permission, live capture,
the visible-border preference, and cancellation while permission is pending.

After both Release builds are complete, `tools\Package.ps1` produces the ARM64
and x64 portable ZIPs and a source ZIP in `dist`.

The provided presets select Visual Studio 2022. With Visual Studio 2026, use
`cmake -S . -B build\arm64 -G "Visual Studio 18 2026" -A ARM64` (or `-A x64`
with `build\x64`), then build that directory with `--config Release`.

## Build on macOS

Use a Mac with Xcode and its command-line tools selected. No third-party Swift
packages are required:

```sh
swift test --configuration release
bash macOS/build.sh --arch universal
```

The output is `dist/PaperShade.app` and a versioned macOS ZIP. The build script
also accepts `--arch arm64` and `--arch x86_64`. `VERSION` is the shared release
version for the Windows executable and macOS bundle.

## Continuous integration and validation scope

[Native platform builds](https://github.com/pebaum/PaperShade/actions/workflows/platforms.yml)
uses four native runner configurations: Windows x64, Windows ARM64, macOS
Intel, and macOS Apple silicon. It builds and tests the portable C++ core on
both operating systems, builds the platform apps, and publishes ZIP artifacts.
The Apple silicon job packages a universal macOS application.

Windows CI uses WARP only for offscreen shader tests; the shipping Windows app
still requires a hardware GPU for advanced styles. macOS Metal tests explicitly
skip if a hosted VM provides no Metal device. The CPU reference tests still
run. CI does not bypass or pre-grant Screen Recording permission.

Real-display capture, Retina/mixed-DPI multi-monitor layouts, sleep/wake,
Spaces/fullscreen interaction, protected content, and OS permission prompts
also need hands-on validation on the target hardware. A successful hosted build
is not a claim that every GPU/display/OS configuration has been exercised.

### Windows command line

```powershell
.\PaperShade.exe --enable
.\PaperShade.exe --pause
.\PaperShade.exe --toggle
.\PaperShade.exe --preset ps1-gray
.\PaperShade.exe --preset ink4
.\PaperShade.exe --fps 15
.\PaperShade.exe --pixel-size 1
.\PaperShade.exe --quit
.\PaperShade.exe --safe
.\PaperShade.exe --reset-settings
```

Commands are forwarded to the existing instance. `--safe` starts paused.
`--reset-settings` pauses and resets only PaperShade preferences; it does not
remove startup registration or reset other accessibility applications.

### Source layout

`src/Main.cpp` owns the Windows tray, hotkeys, power/session events, and error
boundary. `src/ColorEffect.cpp` owns reversible compositor transforms and the
recovery helper. `src/CaptureEngine.cpp` owns WGC capture and overlay lifetime;
`src/GpuRenderer.cpp` and `src/Filter.hlsl` implement the D3D11 pass.

`src/Filters.h` is the platform-independent parameter/reference implementation.
`core/` exposes a small, validated C ABI for Swift and tests. `macOS/` contains
the native AppKit app, ScreenCaptureKit lifecycle, Metal renderer, Swift tests,
and bundle packaging. `.github/workflows/platforms.yml` builds the native
architectures; generated binaries, signing credentials, and build directories
are excluded from Git.
