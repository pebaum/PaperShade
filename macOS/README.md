# PaperShade for macOS

Native AppKit menu-bar application for macOS 13 or later, on Apple silicon and
Intel Macs with Metal support. There is no Dock icon, Electron, WebView,
Accessibility keyboard monitoring, network traffic, telemetry, saved frame, or
audio capture.

## Use and permission

1. Extract `PaperShade-<version>-macos-<arch>.zip` and move `PaperShade.app` to
   Applications. `universal` contains both `arm64` and `x86_64`.
2. Open the app. A new profile starts **paused**; find the half-shaded circle in
   the menu bar. Previously chosen settings, including an explicit enable, are
   restored using UserDefaults.
3. Choose **Enable PaperShade**, or **Grant Screen Recording Access**.
   Only these explicit user actions can request macOS consent.
4. In **System Settings → Privacy & Security → Screen Recording**, enable
   PaperShade. Newer macOS releases may call this **Screen & System Audio
   Recording**. Quit and reopen the app if macOS requests it, then enable the
   filter again. The menu contains a link to these settings.

Apple's screen-recording/sharing indicator stays visible. PaperShade does not
alter privacy preferences, gamma ramps, ICC profiles, or private display APIs.
Changing an ad-hoc-signed build or moving it can require granting permission
again. Launching a loose executable with `swift run` can associate consent with
the terminal instead; use the packaged app for normal use.

### Controls

| Control | Behavior |
| --- | --- |
| Control–Option–G | Toggle enabled/paused |
| Control–Option–Shift–G | Always pause, including during startup |
| Style | All 12 preset IDs match the Windows/shared core |
| Frame Rate | 10, **15 default**, 30, or 60 fps |
| Dither Pattern Size | **1 backing pixel** accurate PS1; 2–4 enlarged artistic patterns |
| Start at Login | Explicit opt-in through `SMAppService`; approval may be needed in Login Items |
| Error / Permission Info | Capture, permission, and shortcut-registration problems |

Carbon global hotkeys do not require Accessibility access. Registration
conflicts are shown in the menu and Error Info, rather than silently ignored.
The menu controls always remain available. Start at Login is never enabled
automatically and can be disabled in the app or System Settings.

The first six styles are grayscale transforms (Natural/Rec.709,
Classic/Rec.601, Average, Paper, High Contrast, Inverted), followed by E-ink
threshold, Bayer black/white, 4 shades, 16 shades, PS1 grayscale and PS1 color.
Unlike the Windows matrix path, **all Mac styles require GPU screen capture**.

## Capture, performance, and limitations

- One ScreenCaptureKit stream and click-through, nonactivating overlay per
  desktop display, matched by display ID to `NSScreen`.
- Capture/drawable sizes use backing pixels, including Retina/scaled modes.
  Texture reads use integer destination coordinates without filtering.
- The current application is excluded with `SCContentFilter`, covering all
  overlay windows. Failure to identify/exclude it aborts capture safely.
  `NSWindow.sharingType` is only an additional defense.
- Frames must be complete and correctly sized. Cursor capture is disabled so
  the actual cursor stays responsive. Static/idle frames cause no redraw loop.
- Each stream uses `minimumFrameInterval`, queue depth 3 and at most one GPU
  command in flight. Other frames are dropped under backpressure. There is one
  render pass, with an IOSurface/CVMetalTextureCache source and no CPU readback.
  Pixel-buffer/texture ownership continues through GPU completion.
- Overlays remain hidden until their first successfully rendered frame. Pause,
  quit, errors, sleep, session switching and monitor changes immediately hide
  and release them. Startup generations/cancellation prevent stale work from
  reopening them. Wake/topology changes use one-shot debounce, not polling.
- Each overlay joins Spaces and supports fullscreen auxiliary placement;
  system-controlled fullscreen/secure surfaces and other unusually elevated
  windows can remain unfiltered. The overlay stays below system menus.
- This is an **SDR, BGRA8/sRGB overlay**, not a display color-management driver.
  HDR/EDR cannot be preserved; protected video can be black/omitted and macOS
  may deny capture. The cursor, recording indicator, menus and secure UI may
  remain unfiltered. Other recording/sharing applications may capture the
  filtered overlay despite PaperShade excluding itself from its own streams.
- Higher resolution, more displays and higher fps increase GPU/memory/power
  use. Pausing releases streams and overlays. No hidden 60-fps display-link or
  CPU pixel processing runs while idle.

The original PS1 signed 4×4 dither is applied in encoded 8-bit channel space,
then clamped, shifted to 5 bits and expanded by bit replication. Only pattern
size 1 is pixel-accurate. Tables and the 48-byte scalar uniform ABI come from
`FilterCore`; Metal fast math and floating-point contraction are disabled.

## Native builds and tests

Use a selected Xcode/command-line developer toolchain with Swift 5.9 or newer:

```bash
# Run on BOTH native architectures. GPU tests explicitly skip only if no
# Metal device is exposed; the CPU contract/exhaustive tests always execute.
swift test

# Universal is the default, including cross-compilation of the other slice.
bash macOS/build.sh
bash macOS/build.sh --arch arm64
bash macOS/build.sh --arch x86_64
bash macOS/build.sh --arch universal

# No recording permission prompt, capture session, hotkeys, or run loop:
dist/PaperShade.app/Contents/MacOS/PaperShade --smoke-test
dist/PaperShade.app/Contents/MacOS/PaperShade --version
```

Build outputs:

- `.build/macos-arm64` and `.build/macos-x86_64`: separate Release scratch paths
- `dist/PaperShade.app`: bundled app with generated native icon and version from
  the root `VERSION`
- `dist/PaperShade-<version>-macos-<arch>.zip`: `ditto` archive preserving the app

System tools generate the icon, `lipo` joins universal slices, and `codesign`
ad-hoc signs and verifies the result. No package dependencies or downloaded
build tools are used. Shader source is embedded, so there is no missing
SwiftPM shader-resource bundle to copy.

**These builds are ad-hoc signed, not Developer ID signed or notarized.**
Gatekeeper can block a downloaded build; use macOS's per-app **Open Anyway**
workflow in Privacy & Security after inspecting/trusting its origin, or build
locally. Do not disable Gatekeeper globally. Notarized distribution requires a
Developer ID certificate, hardened-runtime signing, notarization and stapling
in a separately configured release process.

Tests exercise every preset/ABI field, malformed settings/C inputs, palette
and dither goldens, and every 8-bit channel value × 16 dither phases × four
pattern sizes on the CPU. GPU tests render real Metal textures against the C
oracle (all quantized output must be exact, especially PS1 RGB; continuous
grayscale allows one byte for UNORM tie rounding). They also exercise the
IOSurface texture-cache path. CPU readback is confined to these tests. A
GPU-less CI skip is not GPU validation; physical-Mac testing is still needed
for permissions, fullscreen/Spaces, sleep/wake, display hot-plug and end-to-end
visual behavior.
