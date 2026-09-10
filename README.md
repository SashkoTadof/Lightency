# Lightency

Native Windows 11 taskbar customization with a minimal footprint and zero latency.

**Windows 11 · x64 · Native C++20 · Portable**

> Customization should never cost you input latency or FPS.

<p align="center">
  <img src="assets/screenshots/main.png" width="360" alt="Lightency settings">
  <img src="assets/screenshots/dock-animation.png" width="360" alt="Dock animation settings">
</p>

## Features

- **Clear taskbar**: Instant transparent taskbar with optional border removal.
- **Fluid dock animation**: macOS-style dock hover magnification and icon click bounce.
- **Fine-tuned physics**: Adaptive automatic or customized curve, wave radius, and scale.
- **Tray item control**: Granular visibility settings for system tray elements.
- **Movable Start button**: Adjust or fix the Start button position.
- **Lightweight & Portable**: Single standalone executable, no background electron/browser runtimes, no installer.

## How it works

Unlike traditional mods that rely on downloading external debugging symbols (PDBs) or hooking undocumented internal functions, Lightency connects directly through the native **WinRT XAML Diagnostics** engine (InitializeXamlDiagnosticsEx):
- **Instant startup**: Element discovery completes in ~4 ms.
- **Zero network overhead**: No symbol downloads, no Microsoft symbol server dependency, 100% offline.
- **Broad compatibility**: Seamless operation across various Windows 11 builds and PC configurations.
- **Clean lifecycle**: All hooks and event subscriptions are gracefully detached when exiting, restoring the standard taskbar immediately.

## Installation

1. Download Lightency-1.0.1-win-x64.zip from [Releases](https://github.com/SashkoTadof/Lightency/releases).
2. Extract the archive into any folder.
3. Launch lightency.exe.

*(Optional)* Enable **Start with Windows** in Settings for seamless launch on boot.

## Compatibility & Requirements

- **OS**: Windows 11 (x64)
- **Privileges**: Standard user privileges (or Administrator if running elevated apps)
- **Displays**: Full Per-Monitor DPI scaling support (V2)

> **Note:** Unsigned Explorer extensions may occasionally trigger generic antivirus warnings because the extension operates within the Explorer process space. Lightency does not use dangerous injection techniques such as CreateRemoteThread or WriteProcessMemory.

## Building from source

Requirements: Visual Studio 2022 / Build Tools (C++20), Windows SDK (10.0.22621+), CMake 3.20+.

```cmd
build.bat
```

The output portable distribution and ZIP archive will be created in `build\`.

## License

Released under the [MIT License](LICENSE).
