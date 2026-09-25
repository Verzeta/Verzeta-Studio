<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Building Verzeta Studio from source

Verzeta Studio 1.0.0 supports Linux and Windows. Each release publishes a Linux AppImage and a Windows zip; this page explains how to build from source.

---

## Prerequisites

|                                        | Minimum               | Notes                                                                                                                                                       |
| -------------------------------------- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Qt 6                                   | 6.5                   | Modules: Core, Gui, Quick, QuickControls2, Qml, Widgets, Network, Sql, Multimedia, Concurrent, WebSockets                                                   |
| KDE Frameworks 6: Kirigami             | 6.0                   | Used by every QML page                                                                                                                                      |
| KDE Frameworks 6: KSyntaxHighlighting  | 6.0                   | Provides the `org.kde.syntaxhighlighting` QML module used by chat code blocks and the canvas editor                                                         |
| KDE Frameworks 6: KNotifications       | 6.0                   |                                                                                                                                                             |
| KDE Frameworks 6: KArchive             | 6.0                   |                                                                                                                                                             |
| CMake                                  | 3.21                  |                                                                                                                                                             |
| C++ compiler with C++20                | GCC ≥ 11 / Clang ≥ 14 |                                                                                                                                                             |
| SQLite 3 (with FTS5)                   | 3.35                  | Development package required (`find_package(SQLite3)`)                                                                                                      |
| `bubblewrap`                           | any                   | Linux only; used by canvas Run. Without it, or when AppArmor blocks it, Run executes scripts without a sandbox and shows a warning. Windows runs scripts directly. |

---

## Linux

### Ubuntu / Debian

```bash
sudo apt install \
    qt6-base-dev qt6-declarative-dev qt6-multimedia-dev \
    qt6-websockets-dev qt6-sql-sqlite-plugin libsqlite3-dev \
    kf6-kirigami-dev kf6-syntax-highlighting-dev \
    libkf6notifications-dev libkf6archive-dev \
    extra-cmake-modules cmake ninja-build g++ pkg-config \
    bubblewrap
```

### Arch

```bash
sudo pacman -S qt6-base qt6-declarative qt6-multimedia qt6-websockets \
               kirigami syntax-highlighting knotifications karchive \
               sqlite extra-cmake-modules \
               cmake ninja bubblewrap
```

### Fedora

```bash
sudo dnf install qt6-qtbase-devel qt6-qtdeclarative-devel \
                 qt6-qtmultimedia-devel qt6-qtwebsockets-devel sqlite-devel \
                 kf6-kirigami-devel kf6-syntax-highlighting-devel \
                 kf6-knotifications-devel kf6-karchive-devel \
                 extra-cmake-modules cmake ninja-build gcc-c++ \
                 bubblewrap
```

Package names change between distribution releases. If one is not found, search for the matching Qt 6 or KDE Frameworks 6 module from the table above.

### Configure and build

```bash
git clone --recurse-submodules <repository-url>
cd Verzeta-Studio

cmake --preset debug
cmake --build build-debug --parallel
```

### Run from the build tree

```bash
./build-debug/backend/verzeta-studio
```

### Install system-wide (optional)

```bash
sudo cmake --install build-debug
```

This drops the binary at `${CMAKE_INSTALL_PREFIX}/bin/verzeta-studio` (default prefix `/usr/local`). Then launch with:

```bash
verzeta-studio
```

---

## Windows (MSYS2)

### Install dependencies

1. Install [MSYS2](https://www.msys2.org/) to `C:\msys64`.
2. Open **MSYS2 UCRT64** and install the build dependencies:

```bash
pacman -S --needed \
    mingw-w64-ucrt-x86_64-qt6-base \
    mingw-w64-ucrt-x86_64-qt6-declarative \
    mingw-w64-ucrt-x86_64-qt6-multimedia \
    mingw-w64-ucrt-x86_64-qt6-websockets \
    mingw-w64-ucrt-x86_64-qt6-tools \
    mingw-w64-ucrt-x86_64-kirigami \
    mingw-w64-ucrt-x86_64-qqc2-desktop-style \
    mingw-w64-ucrt-x86_64-syntax-highlighting \
    mingw-w64-ucrt-x86_64-knotifications \
    mingw-w64-ucrt-x86_64-karchive \
    mingw-w64-ucrt-x86_64-breeze-icons \
    mingw-w64-ucrt-x86_64-extra-cmake-modules \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-sqlite3
```

### Configure and build (PowerShell)

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake --preset windows-native
cmake --build build-windows --parallel
```

The `windows-native` preset (in `CMakePresets.json`) builds without local llama.cpp inference. The seven remote and server providers (OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek, Ollama, llama.cpp Remote) work without it. Use the `windows-native-localai` preset to build with the built-in engine (Vulkan).

### Run from the build tree

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build-windows\backend\verzeta-studio.exe
```

### Packaging (self-contained bundle)

`packaging\windows\deploy-windows.ps1` creates a self-contained directory with the executable, all Qt and KDE DLLs, QML modules, and icon data, so the target machine does not need MSYS2.

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\packaging\windows\deploy-windows.ps1                       # Bundle only
.\packaging\windows\deploy-windows.ps1 -Zip                   # Bundle + portable zip
.\packaging\windows\deploy-windows.ps1 -BuildInstaller        # Bundle + Inno Setup installer
.\packaging\windows\deploy-windows.ps1 -Zip -BuildInstaller   # All three
```

Inno Setup 6 is available at <https://jrsoftware.org/isdl.php>. The installer script is at `packaging\windows\verzeta-studio.iss`.

---

## macOS

macOS is not supported in 1.0.0.

---

## Build options

| Flag                      | Default | Effect                                                                               |
| ------------------------- | ------- | ------------------------------------------------------------------------------------ |
| `VERZETA_BUILD_TESTS`     | `ON`    | Build the unit + integration test suite                                              |
| `VERZETA_ENABLE_LLAMACPP` | `ON`    | Builds the `verzeta-inference` helper that runs local GGUF models (needs the llama.cpp submodule and the Vulkan SDK, or another backend) |
| `VERZETA_LLAMA_BACKEND`   | `auto`  | `auto` / `vulkan` / `cuda` / `openblas` / `metal` / `cpu` for the vendored llama.cpp |

The `debug` preset turns it off and the `release` preset turns it on. If you do not have the Vulkan SDK installed and do not need the built-in engine, pass `-DVERZETA_ENABLE_LLAMACPP=OFF`. The other seven providers work without it. Without the llama.cpp submodule (clone with `--recurse-submodules`), CMake turns it off with a warning.

### Vector search (sqlite-vec)

At configure time, `cmake/SqliteVec.cmake` downloads the prebuilt `sqlite-vec`
loadable extension for your platform (SHA-256 verified) into the build tree and
stages it next to the executable; it is also covered by the install rules for
packaging. The binary is **never committed** to the repository. If the download
fails, the build continues and vector search uses a slower brute-force scan at
runtime. System `sqlite3` development files
(`pkg-config sqlite3`) are required to enable the runtime loader; without them
the same brute-force fallback applies.

---

## First launch

On first run the app creates its data directory and shows the **Get Started** wizard. The wizard walks you through configuring at least one LLM provider, picking a routing backend for group chats, and a brief tour of multi-agent teams.

Data directory location (OS-specific):

| OS      | Location                                  |
| ------- | ----------------------------------------- |
| Linux   | `~/.local/share/Verzeta/verzeta-studio/`  |
| Windows | `%APPDATA%\Verzeta\verzeta-studio\`       |

The database file, logs, skill folders, project documents, and canvas mirror files all live there. Settings are in `~/.config/Verzeta/verzeta-studio.conf` on Linux and the registry key `HKEY_CURRENT_USER\Software\Verzeta\verzeta-studio` on Windows.

---

## Tests

The unit + integration test suite is the main verification path. Run it from the build tree:

```bash
ctest --test-dir build-debug --output-on-failure
```

A separate stress harness lives under `tests/stress/` and drives the backend against live LLM providers. It is not run in CI; it guards against regressions in the chat orchestration code.
