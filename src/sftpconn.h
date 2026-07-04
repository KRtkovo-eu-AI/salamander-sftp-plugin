// Copyright © 2026 Dupl3xx
//
// SFTP connection layer over libssh2 - core of the Salamander SFTP plugin.
// Independent of Salamander, testable standalone.
#pragma once
#include <winsock2.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <string>
#include <vector>

struct CSftpEntry
{
    std::string Name;
    bool IsDir;
    bool IsLink;                // symbolic link
    unsigned __int64 Size;
    unsigned long MTime;        // unix modification time
    unsigned long Permissions;  // posix mode
    std::string Owner;          // owner (name or UID)
    std::string Group;          // group (name or GID)
};

// Simple synchronous SFTP session. Blocking sockets.
class CSftpConnection
{
public:
    CSftpConnection();
    ~CSftpConnection();

    // Connect, perform SSH handshake and password authentication.
    // Connection. When 'keyFile' is non-empty, uses private key authentication
    // (with optional 'password' passphrase), otherwise password.
    // protocol: 0 = SFTP, 1 = SCP. With SFTP and 'scpFallback', automatically
    // falls back to SCP (shell + scp transfer) when SFTP subsystem is unavailable.
    bool Connect(const char* host, int port, const char* user, const char* password, const char* keyFile = nullptr,
                 bool useCompression = false, int protocol = 0, bool scpFallback = false);
    void Disconnect();
    bool IsConnected() const { return Sftp != nullptr || (ScpMode && Session != nullptr); }
    bool IsScpMode() const { return ScpMode; }

    // List directory (remotePath in "/" or "/dir/sub" style).
    bool ListDir(const char* remotePath, std::vector<CSftpEntry>& out);

    // Download remote file to local. resumeOffset>0 = resume from given position
    // (SFTP only; SCP cannot resume). Returns current remote file size in *remoteSize.
    bool Download(const char* remotePath, const char* localPath, unsigned __int64 resumeOffset = 0);
    // Upload local file to remote. resumeOffset>0 = resume from given position (SFTP only).
    bool Upload(const char* localPath, const char* remotePath, unsigned __int64 resumeOffset = 0);
    // Remote file size (for resume decision). false = does not exist/error.
    bool RemoteFileSize(const char* remotePath, unsigned __int64& size);

    // Execute command on server (SSH exec) and return its output.
    bool ExecCommand(const char* command, std::string& output);

    // Security information about connection (host key fingerprint, ciphers).
    bool GetSecurityInfo(std::string& out);

    bool MakeDir(const char* remotePath);
    bool RemoveDir(const char* remotePath);
    bool RemoveFile(const char* remotePath);
    bool Rename(const char* oldPath, const char* newPath);

    // Path type: 0 = does not exist, 1 = file, 2 = directory.
    int PathType(const char* remotePath);

    // Change unix permissions (chmod). mode = posix permissions (e.g. 0755).
    bool Chmod(const char* remotePath, unsigned long mode);
    // Load current permissions. Returns false on error.
    bool GetPermissions(const char* remotePath, unsigned long& mode);
    // Full info about path (size, permissions, uid, gid, mtime). Returns false on error.
    bool StatFull(const char* remotePath, unsigned __int64& size, unsigned long& perms,
                  unsigned long& uid, unsigned long& gid, unsigned long& mtime);

    const char* LastError() const { return ErrorMsg.c_str(); }

    // Progress callback: called during transfer. Returns true = continue, false = abort.
    // done/total are bytes of the current file.
    typedef bool (*ProgressFn)(void* ctx, const char* name, unsigned __int64 done, unsigned __int64 total);
    static void SetProgressCallback(ProgressFn cb, void* ctx) { Progress = cb; ProgressCtx = ctx; }

    // Host key verification. status: 0 = unknown server, 1 = CHANGED key (possible attack).
    // Return: 0 = reject, 1 = trust once, 2 = trust and save.
    typedef int (*HostKeyVerifyFn)(void* ctx, const char* host, int port, const char* keyType,
                                   const char* sha256fp, int status);
    static void SetHostKeyCallback(HostKeyVerifyFn cb, void* ctx) { HostKeyCb = cb; HostKeyCtx = ctx; }
    static void SetKnownHostsFile(const char* path);

    // Keyboard-interactive: callback fills response to prompt. echo=display typed text.
    // Return false = user cancelled.
    typedef bool (*KbdPromptFn)(void* ctx, const char* prompt, bool echo, char* out, int outSize);
    static void SetKbdPromptCallback(KbdPromptFn cb, void* ctx) { KbdCb = cb; KbdCtx = ctx; }

    // name encoding: 0 = Auto/UTF-8, 1 = UTF-8, 2 = Off (no conversion)
    static void SetEncoding(int e) { Encoding = e; }

    static bool GlobalInit();   // libssh2_init + WSAStartup
    static void GlobalExit();

private:
    void SetError(const char* ctx);

    // --- SCP mode (via shell commands + libssh2_scp_*) ---
    bool ScpMode;
    // execute command, return output and (optionally) exit code
    bool ExecRaw(const char* command, std::string& out, int* exitCode);
    // execute command, return true if it finished with code 0
    bool ExecSimple(const char* command);
    // SCP operation variants
    bool ScpListDir(const char* remotePath, std::vector<CSftpEntry>& out);
    bool ScpDownload(const char* remotePath, const char* localPath);
    bool ScpUpload(const char* localPath, const char* remotePath);

    // verify host key against known_hosts and optionally ask the user (callback)
    bool VerifyHostKey(const char* host, int port);
    // authentication: password / key / keyboard-interactive (tries based on server offerings)
    bool Authenticate(const char* user, const char* password, const char* keyFile);
    // load PuTTY .ppk key and authenticate (conversion via OpenSSL). Returns: 1=ok, 0=error, -1=not ppk
    int TryPpkAuth(const char* user, const char* keyFile, const char* passphrase);

    static HostKeyVerifyFn HostKeyCb;
    static void* HostKeyCtx;
    static KbdPromptFn KbdCb;
    static void* KbdCtx;
    static std::string KnownHostsFile;
    static int Encoding;
    // conversions between display (ANSI) and server (UTF-8) paths/names
    std::string ToServerEnc(const char* in);  // ANSI -> UTF-8 (for paths sent to server)
    std::string ToDisplayEnc(const char* in); // UTF-8 -> ANSI (for names from server)

    SOCKET Sock;
    LIBSSH2_SESSION* Session;
    LIBSSH2_SFTP* Sftp;
    std::string ErrorMsg;

    static ProgressFn Progress;
    static void* ProgressCtx;
    bool ReportProgress(const char* name, unsigned __int64 done, unsigned __int64 total);
};
