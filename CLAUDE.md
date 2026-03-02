# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### macOS
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/RemoteDesktop.app/Contents/MacOS/RemoteDesktop           # run (dev, uses Homebrew dylibs)

# Full deploy (rebuilds, bundles all dylibs, installs to /Applications):
./deploy-macos.sh

# The .app is self-contained — no Homebrew needed on target Mac
```

### Windows (cross-build via SSH to jumpbox)
```bash
# Full build + deploy (builds, copies DLLs, installs to Program Files, creates desktop shortcut):
./deploy-windows.sh

# Installs to: C:\Program Files\RemoteDesktop\RemoteDesktop.exe
# Desktop shortcut: RemoteDesktop.lnk
```

#### Manual steps (if not using deploy script)
```bash
# Copy source (no rsync on Windows)
scp -r src CMakeLists.txt resources jumpbox:'C:/remotedesktop/'

# Configure
ssh jumpbox "cd /d C:\remotedesktop && rmdir /s /q build && mkdir build && cd build && cmake .. \
  -G \"Visual Studio 17 2022\" -A x64 \
  -DCMAKE_PREFIX_PATH=\"C:/Qt/6.8.3/msvc2022_64;C:/freerdp;C:/vcpkg/installed/x64-windows\" \
  -DLIBVTERM_INCLUDE_DIR=C:/libvterm/include \
  -DLIBVTERM_LIBRARY=C:/libvterm/lib/vterm.lib"

# Build
ssh jumpbox "cd /d C:\remotedesktop\build && cmake --build . --config Release"

# Deploy DLLs (use robocopy — CMD copy breaks over SSH due to backslash escaping)
ssh jumpbox "C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe C:\remotedesktop\build\Release\RemoteDesktop.exe"
ssh jumpbox "robocopy C:\freerdp\bin C:\remotedesktop\build\Release *.dll /NP /NFL /NDL" || true
ssh jumpbox "robocopy C:\vcpkg\installed\x64-windows\bin C:\remotedesktop\build\Release libssh2.dll libssl-3-x64.dll libcrypto-3-x64.dll zlib1.dll /NP /NFL /NDL" || true

# Install to Program Files
ssh jumpbox "robocopy C:\remotedesktop\build\Release \"C:\Program Files\RemoteDesktop\" /MIR /NP /NFL /NDL" || true
```

No tests or linting infrastructure exists yet.

## Architecture

**C++20 / Qt 6 desktop app** for managing RDP and SSH connections. Dual-protocol: FreeRDP 3 for RDP, libssh2+libvterm for SSH terminal emulation.

### Dual-Database Design

Two SQLite databases replace the old single `connections.db`:

- **Shared DB** (configurable path via `config.ini`) — folders and connections only, no credentials. Admin: read-write; non-admin: read-only.
- **User DB** (`~/.remotedesktop/user.db`) — credentials, vault metadata, credential assignments (connection_id→credential_id), folder credential defaults. Each OS user owns theirs.

`~/.remotedesktop/config.ini` stores `shared_database` path and `admin` flag. The `Application` singleton orchestrates both databases.

### Key Layers

- **`src/app/Application`** — Singleton. Owns `ConfigManager`, `ConnectionDatabase` (shared), `UserDatabase` (per-user), `CredentialVault`. `init(argc, argv)` parses CLI, opens both DBs, creates vault.
- **`src/core/connectiondb/`** — Shared DB schema and CRUD. No credential columns — those were moved to UserDatabase.
- **`src/core/userdb/`** — User DB schema and CRUD. Credential assignments, folder defaults, `propagateCredentials()` walks shared DB tree writing to user DB tables.
- **`src/core/credentials/CredentialVault`** — AES-256-GCM encryption, PBKDF2 key derivation (600k iterations). Takes `UserDatabase*` (not ConnectionDatabase).
- **`src/core/rdp/`** — `RdpSession` runs FreeRDP on a background thread, emits framebuffer updates. `RdpClient` holds C callbacks.
- **`src/core/ssh/`** — `SshSession` runs libssh2 on a background thread. `SshChannel` manages terminal I/O via libvterm.
- **`src/ui/treeview/`** — `ConnectionTreeModel` (QAbstractItemModel) loads from shared DB and overlays user DB credential assignments/folder defaults. `ConnectionTreeView` handles drag-drop, context menus, Shift+Arrow reordering — all gated on `isAdmin()`.
- **`src/main.cpp`** — Startup flow: config check → legacy migration or SetupDialog → dual DB init → master password → MainWindow. Signal wiring splits shared-DB writes (admin-only) from user-DB writes (everyone).

### Admin Gating

Non-admins can: view the tree, connect sessions, manage their own credentials, assign credentials to connections, set folder defaults, force-inherit credentials.

Non-admins cannot: add/delete/rename folders or connections, reorder items, drag-drop, import/export.

### Data Flow for Connections

When connecting: `main.cpp` reads `entry.credentialId` (overlaid from user DB) → fetches credential from `userDb->credentialById()` → decrypts via vault → passes plaintext to `RdpSessionWidget` or `SshSessionWidget`.

## Key Patterns and Gotchas

- `VTERM_KEY_FUNCTION()` macro returns `int` in C++ — needs `static_cast<VTermKey>()`
- `Application.h` uses `std::unique_ptr` with forward-declared types — needs explicit destructor in .cpp
- FreeRDP `freerdp` is a typedef, not a struct — don't use `struct freerdp*`
- PIXEL_FORMAT_BGRA32 in FreeRDP maps to QImage::Format_ARGB32 on little-endian
- FreeRDP 3.x channel loading (static builds): `freerdp_load_channel_addin_entry` without a registered provider goes straight to dlopen, which fails. Register a custom provider via `freerdp_register_addin_provider()` that delegates to `freerdp_channels_load_static_addin_entry()` (for SVCs) and `freerdp_channels_client_find_static_entry("DVCPluginEntry", name)` (for DVCs like rdpgfx). Load SVCs (cliprdr, drdynvc) via `freerdp_channels_client_find_static_entry("VirtualChannelEntryEx", name)` + `freerdp_channels_client_load_ex()`. Register DVCs via `freerdp_client_add_dynamic_channel()`. PubSub `ChannelConnected` events deliver the channel interface.
- FreeRDP GFX pipeline requires `update->DesktopResize` callback set in `rdp_post_connect`; `gdi_ResetGraphics` asserts it's non-NULL. The handler must call `gdi_resize()` then update `RdpSession` buffer pointers.
- Windows SDK `rpcndr.h` defines `small` as a macro — collides with libvterm's `VTermScreenCellAttrs::small`. Use `#pragma push_macro("small")/#undef small` around `#include <vterm.h>`.
- RDP keyboard: macOS uses `nativeVirtualKey()` (Carbon keycodes) + lookup table; Windows uses `nativeScanCode()` directly (hardware scan codes = RDP scan codes). Guard with `#ifdef Q_OS_MACOS`.
- Qt `Qt::META` = Cmd on macOS but Win key on Windows. Use `Qt::CTRL` for cross-platform shortcuts or `QKeySequence::StandardKey` enums.
- SQLite concurrent access: WAL mode + `PRAGMA busy_timeout=5000` on shared DB

## Dependencies

| Library | macOS | Windows | Purpose |
|---------|-------|---------|---------|
| Qt 6 | 6.10.2 Homebrew `/opt/homebrew/opt/qt` | 6.8.3 `C:\Qt\6.8.3\msvc2022_64` | UI framework + SQL |
| FreeRDP 3 | 3.12.0 custom build `/opt/homebrew/opt/freerdp3-custom` | Pre-built DLLs `C:\freerdp` | RDP client |
| libssh2 | 1.11.1 Homebrew | vcpkg `C:\vcpkg\installed\x64-windows` | SSH client |
| libvterm | 0.3.3 Homebrew | Built from source `C:\libvterm` | Terminal emulation |
| OpenSSL 3 | Homebrew | vcpkg | Crypto for vault |

FreeRDP CMake package names are `FreeRDP`, `FreeRDP-Client`, `WinPR` (not suffixed with 3). Target names: `freerdp`, `freerdp-client`, `winpr`.
