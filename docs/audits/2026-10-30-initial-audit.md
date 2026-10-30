# Lightency security, performance, and reliability audit

Date: 2026-10-30

## Scope and method

Reviewed the controller, hook DLL, shared configuration, updater, configuration persistence, taskbar and Start Menu integration, drag-and-drop assistance, and window animation paths. Findings are based on source-level data-flow and lifecycle tracing. The final x64 Release build and a basic application-start smoke check were completed; the hook and update behavior paths were not exercised.

## Findings and remediation record

### A-01 — HTTPS download could be redirected to HTTP

- **Severity:** High
- **Evidence:** `src/app/update_manager.cpp`, `DownloadFile`.
- **Cause and impact:** The request originally allowed any redirect after validating only the starting URL, permitting an HTTPS-to-HTTP downgrade of the update archive.
- **Decision and status:** Set `WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP` and fail closed if WinHTTP cannot apply it. **Fixed.** Microsoft documents this policy as rejecting HTTPS-to-HTTP redirects while allowing other redirects.

### A-02 — Shared configuration readers could spin forever

- **Severity:** High
- **Evidence:** `src/common/types.h`, `SharedHookConfig::Read` and writer methods.
- **Cause and impact:** A process exiting during an odd sequence value could leave hook readers spinning indefinitely. The sequence value was also copied with the rest of the payload.
- **Decision and status:** Read the sequence atomically without writing through the read-only mapping, copy only the payload, retry a bounded number of times, and keep the last caller snapshot when no stable read is available. A subsequent owner write normalizes a stale odd value. The first reader implementation used a write-based interlocked operation and caused the confirmed crash documented in A-16; it has been replaced with atomic loads. **Corrected and rebuilt; Explorer runtime remains unverified.**

### A-03 — XAML teardown ran under the loader lock

- **Severity:** High
- **Evidence:** `src/hook/dllmain.cpp` detach behavior and Start Menu unhook paths in `src/app/injector.cpp`.
- **Cause and impact:** WinRT/XAML teardown from `DllMain` could deadlock during unload and leave modified Start Menu elements.
- **Decision and status:** Keep `DllMain` loader-safe; send a shutdown message through the Start Menu hook before unhooking, and perform teardown from a normal hook callback. **Fixed in the standard lifecycle; Windows runtime validation remains.** If the host is hung or does not dispatch the bounded shutdown message, restoration may wait for that host to restart.

### A-04 — Owner liveness checks opened a process handle on every dock render

- **Severity:** Medium
- **Evidence:** `src/hook/dock_animation.cpp`, `EnsureLiveConfigMapped` called by render callbacks.
- **Cause and impact:** `OpenProcess`, `GetExitCodeProcess`, and `CloseHandle` repeated at display-frame frequency despite an existing 250 ms owner-monitor worker.
- **Decision and status:** Read coherent config snapshots and cache owner liveness for one second; the worker remains responsible for prompt teardown. **Fixed.**

### A-05 — Low-level mouse hook performed UI Automation and slept

- **Severity:** Medium
- **Evidence:** `src/hook/drag_drop_assist.cpp`, `MouseHookProc` and activation path.
- **Cause and impact:** Cross-process UI Automation and sleeps inside a system-wide low-level input callback could delay input and risk hook removal.
- **Decision and status:** Queue activation to the Windows thread pool; keep at most one activation outstanding; retain a DLL reference until the callback returns. **Fixed in source; behavior needs Windows runtime validation.**

### A-06 — Updater ignored extraction and installation failures

- **Severity:** Medium
- **Evidence:** `src/app/update_manager.cpp`, `ExecuteUpdatePayload`.
- **Cause and impact:** The old flow ignored the extraction wait and file replacement failures, which could leave a mixed installation.
- **Decision and status:** Check extraction completion and exit status, terminate the child on wait failure, require both staged binaries, and launch a helper from the extracted release. The helper validates the staging path and owner executable, waits up to 30 seconds for the owner to exit, replaces files with rollback, retries a locked hook DLL, then starts the installed app. The owner restores window animation, border, taskbar, and hook state before exiting. **Source-level fix applied; self-update still requires Windows runtime validation.**

### A-07 — Temporary update staging ignored path API failures

- **Severity:** Medium
- **Evidence:** `src/app/update_manager.cpp`, `GetTempUpdateDir` and `StartDownloadWorker`.
- **Cause and impact:** Failed temporary-path, GUID, or directory APIs could feed invalid paths into download and extraction operations; repeated staging could also accumulate files.
- **Decision and status:** Validate each API result, use a correctly sized path buffer, create one GUID-named directory per update, and clean it on cancellation or failure. **Fixed.**

### A-08 — Message loops treated `GetMessageW` failure as a message

- **Severity:** Low
- **Evidence:** Main and hook worker message loops.
- **Cause and impact:** `GetMessageW` returns `-1` on failure, which is truthy and could dispatch an invalid message.
- **Decision and status:** Initialize `MSG`, loop only while the return value is greater than zero, and report the error on the main loop. **Fixed in the reviewed loops.**

### A-09 — Drag-and-drop hook could outlive its DLL

- **Severity:** High
- **Evidence:** `src/hook/drag_drop_assist.cpp`, `Shutdown` and watcher lifecycle; `src/hook/dllmain.cpp`, extension worker teardown.
- **Cause and impact:** Shutdown cleared targets and disabled the feature but did not wake or join the watcher thread, leaving a global mouse hook pointing into DLL code that the extension worker could unload. Queued UI Automation work had the same module lifetime risk.
- **Decision and status:** Stop the watcher, wait for it to remove the hook, and hold a module reference for each outstanding thread-pool callback until callback return. The extension worker is detached and owns its module reference through cleanup. Failed initialization also shuts down Drag & Drop. **Fixed in source; Windows unload and restart paths need runtime validation.**

### A-10 — Controller threads could write shared configuration concurrently

- **Severity:** High
- **Evidence:** `Injector::Update` is called by the UI thread; updater worker calls `Injector::Shutdown` from `src/app/update_manager.cpp`.
- **Cause and impact:** The seqlock has a single-writer assumption. Concurrent `Update` and `Shutdown` could corrupt sequence transitions or race shared mapping teardown.
- **Decision and status:** Serialize injector lifecycle and shared-memory access with a recursive mutex because public injector methods call one another. **Fixed in source.**

### A-11 — Update archive size was not bounded

- **Severity:** Medium
- **Evidence:** Release asset size parsing and `DownloadFile` in `src/app/update_manager.cpp`.
- **Cause and impact:** An absent, malformed, or hostile size field could allow an unbounded download and disk consumption.
- **Decision and status:** Validate the reported size, cap compressed archives at 512 MiB, compare available content length with release metadata, and enforce the limit while streaming. **Fixed for compressed download size.** Expanded archive size is not preflighted, so a crafted compression bomb remains a residual risk.

### A-12 — Updated process could exit behind the single-instance mutex

- **Severity:** High
- **Evidence:** `src/app/update_manager.cpp`, updater launch; `src/app/main.cpp`, single-instance mutex initialization.
- **Cause and impact:** The updater launched the replacement while the old process still owned `Lightency_SingleInstance_Mutex`. The new process could forward its request to the old instance and exit; the old updater then exited too, leaving no running app.
- **Decision and status:** Launch an update helper from staging. It waits for the old process to release the mutex before replacing files and starts the installed app only after the swap. **Fixed in source; runtime handoff validation remains.**

### A-13 — Update-check thread could remain joinable during normal exit

- **Severity:** Medium
- **Evidence:** `src/app/update_manager.cpp`, static `g_checkThread`; normal shutdown path in `src/app/main.cpp`.
- **Cause and impact:** The global `std::thread` was joined only when starting a later check. A completed but still joinable thread could reach its destructor on normal application exit, which calls `std::terminate`.
- **Decision and status:** Detach the check thread during orderly shutdown and again after the message loop as a fallback. **Fixed in source.**

### A-14 — Taskbar styling ran on every pointer-move event

- **Severity:** Medium
- **Evidence:** `src/hook/dock_animation.cpp`, taskbar `PointerMoved` subscription.
- **Cause and impact:** Each mouse movement over the taskbar recursively traversed its XAML visual tree and reassigned appearance and tray visibility properties. This work is also performed when the taskbar is attached and when configuration changes, so repeating it in the pointer hot path could stall Explorer while interacting with the taskbar.
- **Decision and status:** Removed the repeated styling and tray-visibility calls from the pointer callback. The callback still refreshes shared settings and updates dock animation state; styling remains in attach and settings-update paths. **Fixed in source and rebuilt. Runtime performance after this fix is not yet verified.**

### A-15 — Starting a portable copy silently changed the autostart target

- **Severity:** Low
- **Evidence:** `src/app/config.cpp`, `ConfigManager::Load` called `SetAutostart(true)` whenever an autostart entry already existed.
- **Cause and impact:** Launching a test or portable copy rewrote the user's Run entry to that executable, even though the user had not changed the autostart setting. This redirected startup to `.build\Release` during this test.
- **Decision and status:** Removed the write from the read-only load path. The Run entry is now written only when the setting changes through configuration. Restored this machine's entry to the previously running installed executable. **Fixed in source; registry state restored.**

### A-16 — Hook reader wrote to a read-only shared-memory view and crashed Explorer

- **Severity:** Critical
- **Evidence:** Windows Application Error events at 2026-10-30 12:56–12:57 record `Explorer.EXE` faulting in `D:\Projects\Lightency\.build\Release\lightency_hook.dll` at offset `0x25655`. The WER minidump shows `lock cmpxchg dword ptr [rdx],ecx` at the faulting instruction, called from `LightencyHookProc`. `SharedHookConfig::Read` used `InterlockedCompareExchange` on `seq`; hook processes open this mapping with `FILE_MAP_READ` in `src/hook/dock_animation.cpp`, `src/hook/start_button_style.cpp`, and `src/hook/start_menu_size.cpp`.
- **Cause and impact:** `InterlockedCompareExchange` is a read-modify-write operation even when both comparison and exchange values are zero. Running it against the read-only mapped page caused an access violation in Explorer's hook callback. Explorer repeatedly crashed and restarted, removing the taskbar and making the desktop appear frozen.
- **Decision and status:** Replaced the sequence probes with `std::atomic_ref<LONG>::load(std::memory_order_acquire)`, which does not write to the mapped page, while retaining bounded retries and the payload barriers. **Source fix applied and fresh Release build succeeded. The application remains stopped; no runtime retest has been performed.**

## Remaining risks and verification limits

- The updater relies on HTTPS and the release API but does not verify a publisher signature or independently trusted digest. A compromised publishing account or release asset could still supply malicious binaries.
- Archive extraction uses the Windows `tar.exe`; this source audit did not establish a limit on expanded size or independently validate every archive path before extraction.
- Start Menu restoration can be delayed if the host does not process the bounded shutdown message. Explorer, Start Menu, hook unload, and self-update flows need runtime checks on supported Windows 11 builds.
- Build: `cmake -S . -B .build -G "Visual Studio 18 2026" -A x64` succeeded, and the initial plus post-A14/A15/A16 x64 Release builds completed with MSVC 19.51 and Windows SDK 10.0.26100.0. The first build exposed two compile errors in edited hook code; both were corrected. Existing SDK and source warnings remain.
- Runtime incident follow-up (2026-10-30): the user reported severe lag and missing taskbar after the new build launch. Windows recorded repeated Explorer crashes in `.build\Release\lightency_hook.dll`; the dump confirms a write-based atomic read against the read-only shared-memory view (A-16). The new controller was closed and the hook unloaded; the user reported the shell recovered after restarting Explorer. The earlier installed process PID 9480 was also stopped during initial triage.
- The A-16 source fix has been built but not run. Keep the app stopped until controlled Explorer hook validation is safe. Explorer, Start Menu, hook teardown, and update flows need runtime validation. No automated tests were run.
- Git was initialized after the source edits. There is no baseline commit, so no pre-audit repository diff is available; the changes are currently uncommitted.
