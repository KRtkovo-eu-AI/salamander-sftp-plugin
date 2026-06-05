// Copyright © 2026 Dupl3xx
//
// SFTP připojovací vrstva nad libssh2 — jádro Salamander SFTP pluginu.
// Nezávislé na Salamanderu, testovatelné samostatně.
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
    bool IsLink;                // symbolický odkaz
    unsigned __int64 Size;
    unsigned long MTime;        // unix čas modifikace
    unsigned long Permissions;  // posix mode
    std::string Owner;          // vlastník (jméno nebo UID)
    std::string Group;          // skupina (jméno nebo GID)
};

// Jednoduchá synchronní SFTP session. Blokující sockety.
class CSftpConnection
{
public:
    CSftpConnection();
    ~CSftpConnection();

    // Připojí, provede SSH handshake a heslovou autentizaci.
    // Připojení. Když je 'keyFile' neprázdné, použije autentizaci privátním klíčem
    // (s případnou frází 'password'), jinak heslo.
    // protocol: 0 = SFTP, 1 = SCP. Při SFTP a 'scpFallback' se při nedostupném
    // SFTP subsystému automaticky přepne na SCP (shell + scp přenos).
    bool Connect(const char* host, int port, const char* user, const char* password, const char* keyFile = nullptr,
                 bool useCompression = false, int protocol = 0, bool scpFallback = false);
    void Disconnect();
    bool IsConnected() const { return Sftp != nullptr || (ScpMode && Session != nullptr); }
    bool IsScpMode() const { return ScpMode; }

    // Výpis adresáře (remotePath ve stylu "/" nebo "/dir/sub").
    bool ListDir(const char* remotePath, std::vector<CSftpEntry>& out);

    // Stažení vzdáleného souboru do lokálního. resumeOffset>0 = pokračuj od dané pozice
    // (jen SFTP; SCP resume neumí). Vrací aktuální velikost vzdáleného souboru v *remoteSize.
    bool Download(const char* remotePath, const char* localPath, unsigned __int64 resumeOffset = 0);
    // Nahrání lokálního souboru na vzdálený. resumeOffset>0 = pokračuj od dané pozice (jen SFTP).
    bool Upload(const char* localPath, const char* remotePath, unsigned __int64 resumeOffset = 0);
    // Velikost vzdáleného souboru (pro rozhodnutí o resume). false = neexistuje/chyba.
    bool RemoteFileSize(const char* remotePath, unsigned __int64& size);

    // Spustí příkaz na serveru (SSH exec) a vrátí jeho výstup.
    bool ExecCommand(const char* command, std::string& output);

    // Bezpečnostní informace o spojení (otisk host key, šifry).
    bool GetSecurityInfo(std::string& out);

    bool MakeDir(const char* remotePath);
    bool RemoveDir(const char* remotePath);
    bool RemoveFile(const char* remotePath);
    bool Rename(const char* oldPath, const char* newPath);

    // Typ cesty: 0 = neexistuje, 1 = soubor, 2 = adresář.
    int PathType(const char* remotePath);

    // Změna unixových práv (chmod). mode = posix práva (např. 0755).
    bool Chmod(const char* remotePath, unsigned long mode);
    // Načte aktuální práva. Vrací false při chybě.
    bool GetPermissions(const char* remotePath, unsigned long& mode);
    // Plné info o cestě (velikost, práva, uid, gid, mtime). Vrací false při chybě.
    bool StatFull(const char* remotePath, unsigned __int64& size, unsigned long& perms,
                  unsigned long& uid, unsigned long& gid, unsigned long& mtime);

    const char* LastError() const { return ErrorMsg.c_str(); }

    // Progress callback: volán během přenosu. Vrací true = pokračovat, false = přerušit.
    // done/total jsou bajty aktuálního souboru.
    typedef bool (*ProgressFn)(void* ctx, const char* name, unsigned __int64 done, unsigned __int64 total);
    static void SetProgressCallback(ProgressFn cb, void* ctx) { Progress = cb; ProgressCtx = ctx; }

    // Ověření host key. status: 0 = neznámý server, 1 = ZMĚNĚNÝ klíč (možný útok).
    // Návrat: 0 = odmítnout, 1 = důvěřovat jednou, 2 = důvěřovat a uložit.
    typedef int (*HostKeyVerifyFn)(void* ctx, const char* host, int port, const char* keyType,
                                   const char* sha256fp, int status);
    static void SetHostKeyCallback(HostKeyVerifyFn cb, void* ctx) { HostKeyCb = cb; HostKeyCtx = ctx; }
    static void SetKnownHostsFile(const char* path);

    // Keyboard-interactive: callback vyplní odpověď na výzvu. echo=zobrazovat psaný text.
    // Návrat false = uživatel zrušil.
    typedef bool (*KbdPromptFn)(void* ctx, const char* prompt, bool echo, char* out, int outSize);
    static void SetKbdPromptCallback(KbdPromptFn cb, void* ctx) { KbdCb = cb; KbdCtx = ctx; }

    static bool GlobalInit();   // libssh2_init + WSAStartup
    static void GlobalExit();

private:
    void SetError(const char* ctx);

    // --- SCP režim (přes shell příkazy + libssh2_scp_*) ---
    bool ScpMode;
    // spustí příkaz, vrátí výstup a (volitelně) návratový kód
    bool ExecRaw(const char* command, std::string& out, int* exitCode);
    // spustí příkaz, vrátí true pokud skončil s kódem 0
    bool ExecSimple(const char* command);
    // SCP varianty operací
    bool ScpListDir(const char* remotePath, std::vector<CSftpEntry>& out);
    bool ScpDownload(const char* remotePath, const char* localPath);
    bool ScpUpload(const char* localPath, const char* remotePath);

    // ověří host key proti known_hosts a případně se zeptá uživatele (callback)
    bool VerifyHostKey(const char* host, int port);
    // autentizace: heslo / klíč / keyboard-interactive (zkouší dle nabídky serveru)
    bool Authenticate(const char* user, const char* password, const char* keyFile);
    // načte PuTTY .ppk klíč a autentizuje (převod přes OpenSSL). Vrací: 1=ok, 0=chyba, -1=není ppk
    int TryPpkAuth(const char* user, const char* keyFile, const char* passphrase);

    static HostKeyVerifyFn HostKeyCb;
    static void* HostKeyCtx;
    static KbdPromptFn KbdCb;
    static void* KbdCtx;
    static std::string KnownHostsFile;

    SOCKET Sock;
    LIBSSH2_SESSION* Session;
    LIBSSH2_SFTP* Sftp;
    std::string ErrorMsg;

    static ProgressFn Progress;
    static void* ProgressCtx;
    bool ReportProgress(const char* name, unsigned __int64 done, unsigned __int64 total);
};
