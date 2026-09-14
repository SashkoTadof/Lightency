# Lightency

Native Windows 11 taskbar customization with a minimal footprint and zero latency.

**Windows 11 · x64 · Native C++20 · Portable**

> Customization should never cost you input latency or FPS.

<p align="center">
  <img src="assets/screenshots/main.png" width="300" alt="Lightency settings">
  <img src="assets/screenshots/dock-animation.png" width="300" alt="Dock animation settings">
</p>
<p align="center">
  <img src="assets/screenshots/start-button.png" width="300" alt="Start Button & Color Picker">
  <img src="assets/screenshots/start-menu.png" width="300" alt="Start Menu customization">
</p>

## Features

- **Clear taskbar**: Instant transparent taskbar with optional border removal.
- **Fluid dock animation**: macOS-style dock hover magnification and icon click bounce.
- **Fine-tuned physics**: Adaptive automatic or customized curve, wave radius, and scale.
- **Start Button customization**: Custom icon color with smooth hue slider, accent color sync, custom scaling (50%–180%), and position adjustments.
- **Start Menu control**: Granular visibility toggles for Search box, Pinned apps, Recommended, User profile, Power button, View selector, and Folders, plus simple/advanced sizing.
- **Window animations & borders (beta)**: Fluid window minimize animation effect and window border removal.
- **Tray item control**: Granular visibility settings for system tray elements.
- **Drag-and-drop assist**: Hover a taskbar app while dragging to bring its window forward.
- **Lightweight & Portable**: Native executable and taskbar extension, no Electron/browser runtime or installer.
- **Built-in updater**: Fast in-app update check and release installer.

## How it works

Unlike traditional mods that rely on downloading external debugging symbols (PDBs) or hooking undocumented internal functions, Lightency connects directly through the native **WinRT XAML Diagnostics** engine (InitializeXamlDiagnosticsEx):
- **Instant startup**: Element discovery completes in ~4 ms.
- **Zero network overhead**: No symbol downloads, no Microsoft symbol server dependency, 100% offline.
- **Broad compatibility**: Seamless operation across various Windows 11 builds and PC configurations.
- **Clean lifecycle**: Event subscriptions are detached when exiting, restoring the standard taskbar immediately.

## Installation

1. Download `Lightency-1.1.0-win-x64.zip` from [Releases](https://github.com/SashkoTadof/Lightency/releases).
2. Extract the archive into any folder.
3. Launch `lightency.exe`.

*(Optional)* Enable **Launch at startup** in Settings for seamless launch on boot.

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
