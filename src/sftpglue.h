// Copyright © 2026 Dupl3xx
//
// Glue between Salamander FS and CSftpConnection + POSIX path helpers.
#pragma once
#include "sftpconn.h"

extern CSftpConnection SftpConn; // single global connection

// Connection profile specified in dialog.
struct CSftpProfile
{
    char Host[256];
    int Port;
    char User[128];
    char Password[256];
    char KeyFile[260]; // private key (optional)
    bool UseCompression; // zlib compression
    int Protocol;        // 0 = SFTP, 1 = SCP
    bool ScpFallback;    // fallback to SCP on SFTP failure
    bool Valid;
};
extern CSftpProfile SftpProfile;

// Saved server (profile in connection manager).
#define SFTP_MAX_PROFILES 100
struct CSftpSavedProfile
{
    char Name[128]; // display name in list
    char Host[256];
    int Port;
    char User[128];
    char Password[256];
    char KeyFile[260];
    char Path[260];
    bool UseCompression; // zlib compression
    int Protocol;        // 0 = SFTP, 1 = SCP
    bool ScpFallback;    // emergency SCP
    char Folder[128];    // folder (group) for connections, "" = root
};
extern CSftpSavedProfile SftpProfiles[SFTP_MAX_PROFILES];
extern int SftpProfileCount;
extern char SftpDefaultSession[128]; // default connection name ("As default" option)

#define SFTP_MAX_FOLDERS 64
extern char SftpFolders[SFTP_MAX_FOLDERS][128]; // folder names (including empty)
extern int SftpFolderCount;

// Ensures connection per SftpProfile. Returns TRUE if connected.
bool SftpEnsureConnected(HWND parent);

extern int SftpEncoding; // 0 = Auto/UTF-8, 1 = UTF-8, 2 = Off

// POSIX path helpers (forward slashes, absolute paths start with "/").
void SftpNormalize(char* path);                       // in-place normalize
void SftpJoin(const char* base, const char* name, char* out, int outSize);
void SftpParent(const char* path, char* out, int outSize); // up-dir
bool SftpIsSamePath(const char* a, const char* b);
bool SftpIsRoot(const char* path);

// Simple input dialog (for keyboard-interactive prompts). echo=show text.
// Returns true if user confirmed, false on cancel.
bool SftpInputDialog(HWND parent, const char* prompt, bool echo, char* out, int outSize);

// Plugin menu commands (bypass Salamander 5.0 kernel limits).
void SftpEditFile(HWND parent, const char* remoteDir, const char* fileName);
void SftpSyncDir(HWND parent, const char* remoteDir, const char* localDir, int direction);
void SftpCalcSize(HWND parent, const char* remoteDir, int panel);
