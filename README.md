# SFTP/SCP Plugin for Open Salamander (x64)

A full-featured **SFTP and SCP** client as a file-system plugin for [Open Salamander 5.0](https://github.com/OpenSalamander/salamander) (x64). Built on top of **libssh2 + OpenSSL**, so it supports modern cryptography that the original solution could not handle.

> Copyright © 2026 Dupl3xx
> Based on the SDK demo template of Open Salamander (SPDX headers retained in SDK files). The core implementation (`sftpconn.*`, `sftpglue.*`) is original work.

---

## Features

### Protocols
- **SFTP** (SSH File Transfer Protocol) – default
- **SCP** – directory listing via shell (`ls`/`stat`), file transfer via `libssh2_scp_*`, operations (`mkdir`/`rm`/`mv`/`chmod`) via shell
- **Fallback SCP** – when the server does not have an SFTP subsystem, automatic fallback to SCP

### Cryptography (via OpenSSL backend)
Everything is **negotiated automatically** based on the server:
- Key exchange: curve25519-sha256, ECDH (nistp256/384/521), DH group14/16/18
- Key types: ed25519, ECDSA, RSA (rsa-sha2-256/512)
- Ciphers: ChaCha20-Poly1305, AES-GCM, AES-CTR
- Compression: zlib (optional)

### Authentication
- **Password**
- **Private key** – OpenSSH/PEM and **PuTTY `.ppk`** (RSA + ed25519, v2/v3, including encrypted ones – v2 SHA1/AES, v3 Argon2id; conversion via OpenSSL)
- **Keyboard-interactive** – including 2FA (first prompt is filled with the password, subsequent prompts are handled via a dialog)

### Security
- **Host key verification** against `known_hosts` (`%APPDATA%\OpenSalamander-SFTP\known_hosts`)
- Trust prompt for unknown servers (save / just now / reject), **warning on key change** (MITM)
- Display of SHA256/SHA1 fingerprint, key type, and server banner

### File Operations
- Browsing (permissions / owner / group columns), downloading, uploading
- **Progress with transfer speed**, overwrite prompt
- **Resume interrupted transfers** – resume from a given position (SFTP only)
- Delete, create directory, rename, **change permissions (chmod)**, properties
- **Edit file on server** (F4 – downloads, opens editor, uploads back after saving)
- **Calculate directory size**, command line (SSH exec)

### Dialog and Connection Management
- Login dialog in WinSCP style (category tree)
- **Saved connections**: New / Edit / Delete / Rename / Set as Default
- **Password eye toggle** (switches between asterisks ↔ readable password)
- Remembers protocol, compression, and SCP fallback per connection

### Localization
- Czech and English interface; language modules (`.slg`) for **all 11 languages** of the Salamander build, so no language error or selection dialog appears in any language version of Salamander.

---

## Source Code Structure (`src/`)

### Plugin Core
| File | Purpose |
|------|---------|
| **`sftpconn.h/.cpp`** | **Connection layer over libssh2.** Connect (handshake, host key verification, password/key/`.ppk`/KBI authentication), ListDir, Download/Upload (with resume + progress), SCP operation variants, chmod/stat, host-key verification (known_hosts), PuTTY `.ppk` parser + conversion to PEM via OpenSSL. Independent of Salamander, can be tested standalone. |
| **`sftpglue.h/.cpp`** | **Glue** between Salamander FS and `CSftpConnection`. POSIX path helpers (normalization, join, parent), `SftpEnsureConnected` (registers host-key/KBI callbacks + known_hosts path), KBI input dialog, connection profile `CSftpProfile` + saved connections. |

### Salamander Integration (based on SDK template, heavily rewritten)
| File | Purpose |
|------|---------|
| `sftp.cpp` | Plugin entry point, registration (FS name `dfs`), menus (Edit File F4, Calculate Size, Disconnect), config loading/saving + saved connections to registry. |
| `fs1.cpp` | **Login dialog** (`ConnectDlgProc`) – category tree, pages, password eye toggle, saved connection management, input dialog. FS image list definition. |
| `fs2.cpp` | **FS interface implementation** – ChangePath, ListCurrentPath, copy to/from FS (with resume and overwrite dialogs), Delete, CreateDir, QuickRename, ChangeAttributes (chmod), ShowProperties, ExecuteCommandLine, ShowSecurityInfo, context menu. |
| `menu.cpp` | Plugin menu item handlers (file editing, size calculation, disconnect). |
| `sftp.h` | Shared declarations, `CFSData` (permissions/owner/group for columns), menu constants. |
| `precomp.h/.cpp` | Precompiled header (includes `SFTP_QUIET` – suppresses SDK demo dialogs). |
| `dialogs.*`, `archiver.cpp`, `viewer.cpp`, `thumbldr.cpp` | SDK template leftovers – **inactive** (plugin only registers as FS, not as archiver/viewer). |

### Resources and Project
| File | Purpose |
|------|---------|
| `lang/lang.rc`, `lang.rc2`, `lang.rh` | Dialogs (login, chmod, input, progress) and text strings (Czech). Compiled into `.slg`. |
| `res/fs.ico`, `dir.ico`, `file.ico` | Icons (FS icon taken from the original WinSCP plugin). |
| `versinfo.rh2` | Version, copyright, description (version resource). |
| `vcxproj/sftp.vcxproj` | MSVC plugin project (links libssh2 + libcrypto, delay-load, Czech encoding via `/source-charset:utf-8 /execution-charset:windows-1250`). |
| `vcxproj/lang_sftp.vcxproj` | Language module `.slg` project. |

---

## How It Works (Connection Flow)

1. User opens a path in the FS (`dfs:`) → the **login dialog** appears (`fs1.cpp`).
2. After confirmation, `fs2.cpp::ChangePath` calls `SftpEnsureConnected` (`sftpglue.cpp`).
3. `CSftpConnection::Connect` (`sftpconn.cpp`):
   - TCP connection → **SSH handshake** (libssh2 + OpenSSL).
   - **Host key verification** against `known_hosts`; unknown/changed → user prompt (callback to `sftpglue`).
   - **Authentication**: key (including `.ppk`) → password → keyboard-interactive (based on server offerings).
   - SFTP subsystem, or (SCP / fallback) shell mode.
4. FS operations call `CSftpConnection` methods via `sftpglue` helpers.

### Library Loading
`sftp.spl` has both `libssh2.dll` and `libcrypto-3-x64.dll` as **delay-loaded**. At startup, `GlobalInit` first explicitly loads **our** DLLs from the plugin directory (`LoadLibraryEx` + `LOAD_WITH_ALTERED_SEARCH_PATH`) to avoid using a different version from `PATH`.

---

## Building

**Requirements:** Visual Studio Build Tools 2022 (x64), Open Salamander SDK (this plugin is part of the `salamander/src/plugins/sftp` tree), libssh2 + OpenSSL via vcpkg.

First install dependencies via the central build script:

```powershell
.\tools\vcpkg\build-third-party-libs.ps1 -SftpPlugin
```

This command:
1. clones and bootstraps vcpkg (if not already present),
2. installs `libssh2` + `openssl` (3.x) into `build/vcpkg_installed_sftp/`,
3. copies DLLs for the UnRAR and FTP plugins into `build/libs/` (original function).

Then build the plugin:

```powershell
MSBuild src\vcxproj\sftp.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `sftp.spl` + `lang\english.slg`. Other languages are generated by copying (see `bin/lang/`).

---

## Deployment (`bin/`)

Copy the following into `plugins\sftp\` in the Salamander installation:
- `sftp.spl`
- `lang\*.slg` (11 languages)
- `libssh2.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `z.dll`, `legacy.dll`

---

## Testing (`test/`)

Local test SSH servers in Python (paramiko) + test clients:

| File | Purpose |
|------|---------|
| `sftp_test_server.py` | SFTP server (`127.0.0.1:2222`, `test`/`test`). Env `SFTP_KBI_ONLY=1` = keyboard-interactive only; `SFTP_MODERN_ONLY=1` = modern ciphers only; `SFTP_PORT` = custom port. |
| `sftp_scp_test_server.py` | Server emulating **SCP** mode (shell commands + scp protocol), `127.0.0.1:2223`. |
| `gen_ppk.py` | Generates test PuTTY `.ppk` from an OpenSSH key. |
| `test_conn2.cpp` | SFTP test (connect, list, upload, chmod, stat, exec, security info, key). |
| `test_scp.cpp` | SCP mode test (list, upload, download, mkdir, rename, …). |
| `test_features.cpp` | Host key test (known_hosts), transfer resume, `.ppk` key. |
| `test_kbi.cpp` | Keyboard-interactive authentication test. |

Verified: SFTP and SCP fully functional; modern ciphers (curve25519/ed25519/chacha20) against a modern-only server; host key save+match; resume byte-exact; `.ppk` (RSA); KBI.

---

## What It Does Not Do (Compared to the Original WinSCP Plugin)
Intentionally **SSH-only** (SFTP+SCP). Not included: FTP/FTPS, WebDAV, S3, proxy, speed limiting, directory synchronization, background queue, saved connection folders, EOL/timezone conversion. SSH version is fixed to 2 (libssh2 does not support SSH-1).
