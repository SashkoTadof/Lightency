# Lightency

Windows 11 taskbar and start menu customization in native C++20.

![Lightency settings](assets/screenshots/main.png)

## Features

- **Dock animation**: icon scaling on mouse hover with macOS-style wave expansion and bounce.
- **Transparent taskbar**: clear taskbar background and remove top border line (compatible with modern Windows 11 builds).
- **Start button styling**: custom icon color, accent color matching, size, and position offset.
- **Start menu cleanup**: toggle visibility of search box, pinned items, recommended section, user profile, and power buttons.
- **Drag-and-drop assist**: hover over taskbar icons while dragging a file to bring the target window to foreground via low-level mouse hooks.
- **System tray**: hide individual tray icons (chevron, network, volume, battery, clock).
- **Window animations & borders**: custom minimize animation and exact window border color restoration.
- **Portable**: standalone executables, no installer, no background services.

## Architecture

Lightency consists of two parts:

- `lightency.exe`: Win32 controller, settings GUI, system tray icon, and self-updater.
- `lightency_hook.dll`: injected module running inside `explorer.exe` and `StartMenuExperienceHost.exe`.

### How it works

1. **Injection**: `lightency.exe` sets thread hooks (`SetWindowsHookExW` with `WH_CALLWNDPROC` and `WH_GETMESSAGE`) on taskbar and start menu threads. Hooks initialize lazily on IPC messages without thread creation inside `DllMain`.
2. **XAML access**: `lightency_hook.dll` hooks into the visual tree using the native `InitializeXamlDiagnosticsEx` API, modifying WinRT XAML elements in-process.
3. **IPC**: Settings are synchronized in real-time via lock-free atomic shared memory (`CreateFileMappingW` / `MapViewOfFile` with seqlock).
4. **Updater**: Performs atomic updates via `MoveFileW` with strict HTTPS enforcement, expected size validation, and native JSON parsing.
5. **Exit**: Event handlers and timers are detached, element transforms are restored, and hooks cleanly unregister.

## Download & Usage

1. Get the latest release from [Releases](https://github.com/SashkoTadof/Lightency/releases).
2. Extract the ZIP and run `lightency.exe`.

Settings can be opened from the system tray icon.

## Requirements & Notes

- **OS**: Windows 11 64-bit (including 24H2 / build 26200+).
- **Permissions**: Standard user permissions. Elevated privileges may be required when interacting with administrator windows.

## Build

Requires Visual Studio 2022 (C++20), Windows SDK 10.0.22621+, and CMake 3.20+.

```cmd
build.bat
```

Output files will be generated in `build\`.

## License

[MIT](LICENSE)
