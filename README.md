# PaperShade

Turn your desktop into grayscale, e-ink-style shades, or a PS1-dithered display.
PaperShade lives quietly in the Windows system tray or Mac menu bar.

## Download

**No developer tools or compiling required.**

| Your computer | Click to download |
| --- | --- |
| Most Windows 11 PCs (Intel / AMD) | **[Download for Windows (.exe)](https://github.com/pebaum/PaperShade/releases/download/v1.2.0/PaperShade-1.2.0-windows-x64.exe)** |
| Windows 11 ARM PCs (Snapdragon) | **[Download for Windows ARM64 (.exe)](https://github.com/pebaum/PaperShade/releases/download/v1.2.0/PaperShade-1.2.0-windows-arm64.exe)** |
| Mac, macOS 13+ (Intel or Apple silicon) | **[Download for Mac (.dmg)](https://github.com/pebaum/PaperShade/releases/download/v1.2.0/PaperShade-1.2.0-macos-universal.dmg)** |

The Mac download works on both Intel and M-series Macs.
[All downloads and release notes](https://github.com/pebaum/PaperShade/releases/tag/v1.2.0)

## Get started

**Windows:** Open the downloaded `.exe`. Find the paper icon beside the clock
(it may be under the up-arrow), right-click it, and choose a display style.
No installation is needed.

**Mac:** Open the `.dmg`, drag **PaperShade** into **Applications**, and open it.
Use its menu-bar icon to choose a style. Allow **Screen Recording** in
**System Settings > Privacy & Security** when requested.

These are **unsigned / ad-hoc-signed preview builds**. Windows may show
SmartScreen; the Mac app is not Apple-notarized and may require **Open Anyway**
in Privacy & Security. Only approve a download you trust; don't disable your
system's security protections.

## Controls

| Action | Windows | Mac |
| --- | --- | --- |
| Toggle the filter | Ctrl+Alt+G | Control+Option+G |
| Always pause | Ctrl+Alt+Shift+G | Control+Option+Shift+G |

Choose from 13 styles, including original colors, traditional grayscale, paper-like contrast,
2/4/16-shade e-ink, and PS1 dithering. Frame rate and dither size are adjustable.
Exit from the tray/menu-bar icon to restore the normal display.

## Warmth (Kelvin)

Use **Warmth (Kelvin)** to choose **1000-6500 K** or enter a custom value.
Lower values look warmer; **6500 K is neutral / off**.
Choose **Warmth only (original colors)** for f.lux-style warming without grayscale,
or combine warmth with another style. This is manual, approximate color warming,
not automatic sunset scheduling or monitor calibration.

Processing stays on your computer: no recordings are saved or uploaded.
GPU effects use some power. macOS keeps its recording indicator, and its
preview still needs hands-on validation of live capture and multiple displays.

[Technical guide and build instructions](https://github.com/pebaum/PaperShade/blob/main/docs/DEVELOPMENT.md)
