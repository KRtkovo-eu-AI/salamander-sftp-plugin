// Copyright © 2026 Dupl3xx
//
// Lepidlo mezi Salamander FS a CSftpConnection + POSIX path helpery.
#pragma once
#include "sftpconn.h"

extern CSftpConnection SftpConn; // jedno globální připojení

// Profil připojení zadaný v dialogu.
struct CSftpProfile
{
    char Host[256];
    int Port;
    char User[128];
    char Password[256];
    char KeyFile[260]; // privátní klíč (volitelně)
    bool UseCompression; // zlib komprese přenosu
    int Protocol;        // 0 = SFTP, 1 = SCP
    bool ScpFallback;    // při SFTP: nouzově přepnout na SCP
    bool Valid;
};
extern CSftpProfile SftpProfile;

// Uložený server (profil v "connection manageru").
#define SFTP_MAX_PROFILES 100
struct CSftpSavedProfile
{
    char Name[128]; // zobrazované jméno v seznamu
    char Host[256];
    int Port;
    char User[128];
    char Password[256];
    char KeyFile[260];
    char Path[260];
    bool UseCompression; // zlib komprese
    int Protocol;        // 0 = SFTP, 1 = SCP
    bool ScpFallback;    // nouzově SCP
};
extern CSftpSavedProfile SftpProfiles[SFTP_MAX_PROFILES];
extern int SftpProfileCount;
extern char SftpDefaultSession[128]; // název výchozího spojení (volba "Jako výchozí")

// Zajistí připojení podle SftpProfile. Vrací TRUE pokud připojeno.
bool SftpEnsureConnected(HWND parent);

// POSIX path helpery (dopředná lomítka, absolutní cesty začínající "/").
void SftpNormalize(char* path);                       // in-place normalizace
void SftpJoin(const char* base, const char* name, char* out, int outSize);
void SftpParent(const char* path, char* out, int outSize); // up-dir
bool SftpIsSamePath(const char* a, const char* b);
bool SftpIsRoot(const char* path);

// Jednoduchý vstupní dialog (pro keyboard-interactive výzvy). echo=zobrazovat text.
// Vrací true pokud uživatel potvrdil, false při zrušení.
bool SftpInputDialog(HWND parent, const char* prompt, bool echo, char* out, int outSize);

// Příkazy z menu pluginu (obcházejí limity jádra Salamander 5.0).
void SftpEditFile(HWND parent, const char* remoteDir, const char* fileName);
void SftpCalcSize(HWND parent, const char* remoteDir, int panel);
