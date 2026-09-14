# Lightency

Windows 11 taskbar and start menu customization in native C++20.

![Lightency settings](assets/screenshots/main.png)

## Features

- **Dock animation**: icon scaling on mouse hover with macOS-style wave expansion and bounce.
- **Transparent taskbar**: clear taskbar background and remove top border line.
- **Start button styling**: custom icon color, accent color matching, size, and position offset.
- **Start menu cleanup**: toggle visibility of search box, pinned items, recommended section, user profile, and power buttons.
- **Drag-and-drop assist**: hover over taskbar icons while dragging a file to bring the target window to foreground.
- **System tray**: hide individual tray icons (chevron, network, volume, battery, clock).
- **Window animations & borders**: custom minimize animation and window border toggle (beta).
- **Portable**: standalone executables, no installer, no background runtime or electron bloat.

## Architecture

Lightency consists of two parts:

- `lightency.exe`: Win32 controller, settings GUI, system tray icon, and updater.
- `lightency_hook.dll`: injected module running inside `explorer.exe` and `StartMenuExperienceHost.exe`.

### How it works

1. **Injection**: `lightency.exe` sets thread hooks (`SetWindowsHookExW` with `WH_CALLWNDPROC` and `WH_GETMESSAGE`) on the taskbar and start menu threads. Windows loads `lightency_hook.dll` into target processes upon message dispatch. No `CreateRemoteThread` or `WriteProcessMemory` is used.
2. **XAML access**: `lightency_hook.dll` hooks into the visual tree using the native `InitializeXamlDiagnosticsEx` API. It reads and modifies WinRT XAML elements in-process without downloading symbol files (PDBs).
3. **IPC**: Settings are synced between `lightency.exe` and `lightency_hook.dll` in real time via shared memory (`CreateFileMappingW` / `MapViewOfFile`).
4. **Network**: The core tool and DLL never access the network. Network requests are only made when checking for updates via the GitHub Releases API.
5. **Exit**: On close, event handlers (`PointerMoved`, `CompositionTarget::Rendering`) are detached, element transforms are restored to stock layout, and the DLL unhooks.

## Download & Usage

1. Get the latest release from [Releases](https://github.com/SashkoTadof/Lightency/releases).
2. Extract the ZIP and run `lightency.exe`.

Settings can be opened from the system tray icon.

## Requirements & Notes

- **OS**: Windows 11 64-bit.
- **Permissions**: Runs as a standard user. Administrator privileges may be needed if you interact with elevated windows or if your system restricts UI thread hooks.
- **Antivirus notice**: Unsigned DLLs injected into Explorer via `SetWindowsHookExW` may trigger heuristic false positives in some security software.

## Build

Requires Visual Studio 2022 (C++20), Windows SDK 10.0.22621+, and CMake 3.20+.

```cmd
build.bat
```

Output files will be generated in `build\`.

## License

[MIT](LICENSE)
