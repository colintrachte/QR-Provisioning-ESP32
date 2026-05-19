# Contributing to Robot Provisioning

Thanks for helping improve Robot Provisioning.

This project is a reusable ESP-IDF template for Wi-Fi provisioning, device settings, updates, diagnostics, and control. Contributions should preserve the project’s core goals: clarity, modularity, maintainability, and easy extension for future products.

## Project goals

When contributing, optimize for the following:

- Clear separation between provisioning, networking, storage, OTA/update handling, control, and web UI.
- Small, reusable components with well-defined responsibilities.
- Long-term maintainability by both humans and AI.
- Strong debug visibility and predictable behavior.
- Minimal duplication and safe defaults.
- Preserved existing behavior unless a change is explicitly intended.

This repository is intended to evolve into multiple future products, so prefer extensible foundations over one-off shortcuts.

## Before you start

Before opening an issue or pull request, read the main repository documentation:

- [`README.md`](README.md)
- [`docs/setup.md`](docs/setup.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/protocol.md`](docs/protocol.md)
- [`docs/hardware.md`](docs/hardware.md)
- [`docs/roadmap.md`](docs/roadmap.md)

If your change affects behavior, wiring, boot flow, protocol details, or build configuration, update the relevant docs in the same change set.

## What to contribute

Helpful contributions include:

- Bug fixes.
- Board support additions.
- Improvements to QR-based Wi-Fi provisioning.
- Better captive portal, onboarding, or reconnection flows.
- OTA and update handling improvements.
- Control protocol changes with matching documentation.
- Web UI improvements.
- Diagnostics, logging, and troubleshooting enhancements.
- Doxygen-compatible comments and documentation cleanup.
- Refactoring that improves modularity without changing behavior.

## Architecture expectations

Please keep code aligned with these conventions:

- Use a modular ESP-IDF component-based layout.
- Keep public headers minimal.
- Separate configuration, state, transport, UI, and hardware-specific logic.
- Reuse existing helpers before introducing new abstractions.
- Avoid duplicated logic.
- Keep APIs small and stable.
- Use interfaces that are easy to test, mock, or replace.
- Preserve board portability whenever possible.

For new hardware support, prefer adding a board HAL and configuration layer rather than embedding board-specific logic in shared modules.

## Code style

Follow the project’s style rules:

- Use C and ESP-IDF conventions.
- Use Allman-style braces.
- Use 4-space indentation.
- Use `#pragma once` in headers.
- Prefer explicit and appropriate types.
- Use `const` where it improves safety and clarity.
- Use descriptive names for files, functions, variables, and log tags.
- Avoid clever code when a clear implementation will do.

If you touch shared code, make the implementation easy to read, debug, and extend.

## Documentation rules

Documentation is part of the codebase.

Please follow these standards:

- Every function in every file must have concise, complete Doxygen-compatible comments.
- Document parameters, return values, side effects, ownership, preconditions, postconditions, and error conditions where relevant.
- Add a short module-level comment at the top of each source file.
- Keep comments accurate and specific.
- Do not repeat obvious implementation details.
- Prefer terminology that is consistent across the whole project.
- Keep comments useful for debugging and AI-assisted maintenance.

Public APIs should be documented clearly enough that another developer can understand how to use them without reading internal implementation details.

## Development setup

The project uses PlatformIO with ESP-IDF for the ESP32 target.

Typical workflow:

```bash
git clone <your-fork-url> robot-provisioning
cd robot-provisioning
```

Common VS Code shortcuts:

- `Ctrl+Alt+B` to build.
- `Ctrl+Alt+U` to upload.
- `Ctrl+Alt+S` to open the serial monitor.

If you need to tune build settings, include the relevant PlatformIO configuration, board selection, framework options, monitor settings, and ESP-IDF-specific flags in your pull request description.

## Testing expectations

Before submitting changes, verify them locally.

At minimum, test the following when relevant:

- Build succeeds.
- Firmware flashes successfully.
- Provisioning flow still works.
- SoftAP/captive portal fallback still works.
- Re-provisioning works when the USER button on GPIO0 is held for 3 seconds at boot.
- QR display behavior is correct.
- WebSocket/control behavior still functions.
- OTA/update behavior still works if touched.
- Hardware-specific changes behave correctly on the target board.

For timing, memory, concurrency, ISR, or power-related changes, call those out explicitly in the pull request notes.

## Bug reports

When reporting a bug, include:

- Board model and revision.
- ESP-IDF and PlatformIO configuration.
- Firmware version or commit hash.
- Clear reproduction steps.
- Serial logs or screenshots.
- Network/provisioning details if relevant.
- Any hardware wiring details that might matter.

The more specific your report, the easier it is to reproduce and fix.

## Pull requests

A good pull request should include:

- A short summary of the change.
- Why the change is needed.
- What hardware or setup you tested.
- Any documentation updates.
- Relevant logs, screenshots, or traces if helpful.

For board support or provisioning changes, mention any assumptions about ESP-IDF version compatibility and any wiring or pin mapping details.

## Commit messages

Write commit messages that describe the actual change clearly.

Good examples:

- `Fix captive portal reconnect handling`
- `Add TTGO LoRa32 V1 board support`
- `Document provisioning QR workflow`
- `Improve WebSocket disconnect recovery`

## License

By contributing to this repository, you agree that your contributions will be licensed under the same MIT license as the project.
