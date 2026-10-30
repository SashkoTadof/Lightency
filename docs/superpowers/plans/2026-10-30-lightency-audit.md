# Lightency Security and Performance Audit Plan

> **For agentic workers:** Execute this plan autonomously in the current workspace, recording findings and chosen remediations as they are established.

**Goal:** Identify and fix substantiated security, performance, and reliability issues in Lightency while preserving its native Windows behavior and low idle overhead.

**Architecture:** Review the existing C++20 controller, injected hook DLL, shared memory IPC, updater, configuration, and rendering/event lifecycles in their current architecture. Record each finding with evidence, root cause, impact, and the selected minimal remediation; then re-audit each changed path and adjacent callers.

**Tech Stack:** C++20, Win32, Windows Runtime/XAML, CMake, MSVC.

**Spec:** User request in this conversation: autonomous deep audit, record the best resolution while investigating, apply fixes, then start a new audit.

## Global Constraints

- Keep Lightency a portable Windows 11 x64 C++20 application and hook DLL.
- Preserve feature behavior and avoid idle CPU, memory, or thread overhead.
- Base changes on code evidence and root-cause analysis.
- Do not run or add tests unless requested; use source review and safe build/static checks where available.
- Preserve user files and existing workspace state; this checkout has no `.git` directory.

## Review Focus

- Untrusted updater responses, parsing, download validation, and replacement paths must not permit malformed input or partial updates to corrupt the installation.
- Process injection and shared-memory IPC must target only intended processes and avoid unsafe access or inconsistent state.
- Hook callbacks and XAML event handlers must not block host UI threads, leak, or outlive referenced objects.
- Timers, animation updates, polling, and logging must not consume resources while idle or accumulate work.
- Configuration and filesystem operations must handle missing, malformed, or oversized data without crashes or unintended path access.

---

### Task 1: Establish architecture and audit evidence

**Files:**
- Review: `src/app/*`, `src/hook/*`, `src/common/*`, `CMakeLists.txt`, `build.bat`, `README.md`
- Create/update: `docs/audits/2026-10-30-initial-audit.md`

- [x] Trace updater, config, injection, IPC, hook initialization/shutdown, animation, and XAML event lifecycles.
- [x] Record findings with severity, file/line evidence, root cause, impact, selected fix, and verification approach.

### Task 2: Apply evidence-backed remediations

**Files:** Exact source files identified by Task 1.

- [x] Fix confirmed security, performance, or reliability defects with the smallest complete change that preserves intended behavior.
- [x] Record changed decisions and any rejected alternatives in the audit report.

### Task 3: Re-audit and report

**Files:** Changed source files and `docs/audits/2026-10-30-initial-audit.md`.

- [x] Re-read changed code and its callers, validate cleanup/error paths, and perform safe non-test checks available in this environment. Windows runtime validation is still outstanding.
- [ ] Update findings as fixed, unresolved, or requiring runtime validation, with concrete evidence and limits.
