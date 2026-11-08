# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.1] - 2026-11-08

### Fixed
- BepInEx no longer stops loading when KeepBuilding is installed. 1.0.0 patched `UnityPlayer.dll`'s `GetProcAddress`
  import, which BepInEx's loader (Doorstop) also needs, so Doorstop failed with `Failed to install IAT hook!`. The mod
  now waits for `GameAssembly.dll` to load and hooks `il2cpp_init` there, without touching any imports.

### Added
- Demo video in the README.

## [1.0.0] - 2026-11-05

### Added
- First release.
- The selected building stays on the hammer after placing it, with its rotation, until you pick something else or
  cancel.
- Proxy `dwmapi.dll`: no mod loader required, and it runs next to [NoFocusLoss](https://github.com/Yukitus86/MoonSweepers-NoFocusLoss).
- `KeepBuilding.ini` with `Enabled`, `ConveyorAfterMachine` and `Log`.

[Unreleased]: https://github.com/Yukitus86/MoonSweepers-KeepBuilding/compare/v1.0.1...HEAD
[1.0.1]: https://github.com/Yukitus86/MoonSweepers-KeepBuilding/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/Yukitus86/MoonSweepers-KeepBuilding/releases/tag/v1.0.0
