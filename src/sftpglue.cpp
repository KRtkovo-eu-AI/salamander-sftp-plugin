// Copyright © 2026 Dupl3xx
#include "precomp.h"
#include "sftpglue.h"
#include <string.h>

CSftpConnection SftpConn;
CSftpProfile SftpProfile = {"", 22, "", "", "", false};
CSftpSavedProfile SftpProfiles[SFTP_MAX_PROFILES];
int SftpProfileCount = 0;
char SftpDefaultSession[128] = "";
char SftpFolders[SFTP_MAX_FOLDERS][128];
int SftpFolderCount = 0;
int SftpEncoding = 0; // 0 = Auto (UTF-8), 1 = UTF-8, 2 = Vypnuto (bez konverze)

// dotaz uživatele na důvěru host key serveru
static int SftpHostKeyCallback(void* /*ctx*/, const char* host, int port, const char* type,
                               const char* fp, int status)
{
    HWND parent = SalamanderGeneral->GetMsgBoxParent();
    char msg[1024];
    if (status == 1) // změněný klíč
    {
        _snprintf_s(msg, _TRUNCATE,
                    "VAROVÁNÍ: Host key serveru %s:%d se ZMĚNIL!\n\n"
                    "Typ klíče: %s\nOtisk (SHA256):\n%s\n\n"
                    "Mohlo by jít o útok (MITM). Opravdu se připojit a uložit nový klíč?",
                    host, port, type, fp);
        return SalamanderGeneral->SalMessageBox(parent, msg, "SFTP – změna host key",
                                                MB_YESNO | MB_ICONWARNING) == IDYES
                   ? 2
                   : 0;
    }
    _snprintf_s(msg, _TRUNCATE,
                "Server %s:%d zatím není znám.\n\nTyp klíče: %s\nOtisk (SHA256):\n%s\n\n"
                "Důvěřovat tomuto serveru?\n"
                "Ano = uložit klíč a připojit\nNe = připojit jen tentokrát\nZrušit = odmítnout připojení",
                host, port, type, fp);
    int r = SalamanderGeneral->SalMessageBox(parent, msg, "SFTP – neznámý server",
                                             MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDYES ? 2 : (r == IDNO ? 1 : 0);
}

// keyboard-interactive: výzva serveru (např. kód 2FA) -> zeptej se uživatele
static bool SftpKbdPromptCallback(void* /*ctx*/, const char* prompt, bool echo, char* out, int outSize)
{
    return SftpInputDialog(SalamanderGeneral->GetMsgBoxParent(), prompt, echo, out, outSize);
}

// cesta k souboru known_hosts (v profilu uživatele)
static const char* SftpKnownHostsPath()
{
    static char path[MAX_PATH] = "";
    if (path[0] == 0)
    {
        const char* appdata = getenv("APPDATA");
        if (appdata != nullptr && appdata[0] != 0)
        {
            _snprintf_s(path, _TRUNCATE, "%s\\OpenSalamander-SFTP", appdata);
            CreateDirectoryA(path, nullptr);
            _snprintf_s(path, _TRUNCATE, "%s\\OpenSalamander-SFTP\\known_hosts", appdata);
        }
    }
    return path;
}

bool SftpEnsureConnected(HWND parent)
{
    if (SftpConn.IsConnected())
        return true;
    if (!SftpProfile.Valid || SftpProfile.Host[0] == 0)
    {
        SalamanderGeneral->SalMessageBox(parent, "Není nastaveno SFTP připojení.\nOtevřete cestu sftp: a zadejte server.",
                                         LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return false;
    }
    static bool initialized = false;
    if (!initialized)
    {
        CSftpConnection::GlobalInit();
        CSftpConnection::SetKnownHostsFile(SftpKnownHostsPath());
        CSftpConnection::SetHostKeyCallback(SftpHostKeyCallback, nullptr);
        CSftpConnection::SetKbdPromptCallback(SftpKbdPromptCallback, nullptr);
        initialized = true;
    }
    CSftpConnection::SetEncoding(SftpEncoding); // kódování názvů souborů
    if (!SftpConn.Connect(SftpProfile.Host, SftpProfile.Port, SftpProfile.User, SftpProfile.Password, SftpProfile.KeyFile,
                          SftpProfile.UseCompression, SftpProfile.Protocol, SftpProfile.ScpFallback))
    {
        char buf[600];
        _snprintf_s(buf, _TRUNCATE, "Nelze se připojit k SFTP serveru %s:%d.\n\n%s",
                    SftpProfile.Host, SftpProfile.Port, SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, buf, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return false;
    }
    return true;
}

void SftpNormalize(char* path)
{
    // sjednoť na "/", rozdrob na komponenty, vyřeš "." a ".."
    char tmp[MAX_PATH];
    lstrcpyn(tmp, (path && *path) ? path : "/", MAX_PATH);
    for (char* p = tmp; *p; p++)
        if (*p == '\\')
            *p = '/';

    const char* parts[256];
    int n = 0;
    char* save = NULL;
    char* tok = strtok_s(tmp, "/", &save);
    while (tok != NULL && n < 256)
    {
        if (strcmp(tok, ".") == 0)
        {
            // přeskoč
        }
        else if (strcmp(tok, "..") == 0)
        {
            if (n > 0)
                n--;
        }
        else
            parts[n++] = tok;
        tok = strtok_s(NULL, "/", &save);
    }
    char* out = path;
    *out++ = '/';
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            *out++ = '/';
        int len = (int)strlen(parts[i]);
        memmove(out, parts[i], len); // memmove: zdroj i cíl ve stejném bufferu (tmp byl kopie)
        out += len;
    }
    *out = 0;
}

void SftpJoin(const char* base, const char* name, char* out, int outSize)
{
    char b[MAX_PATH];
    lstrcpyn(b, (base && *base) ? base : "/", MAX_PATH);
    int bl = (int)strlen(b);
    if (bl > 0 && b[bl - 1] == '/')
        b[bl - 1] = 0; // odstraň koncové lomítko (kromě toho že root "" -> spojí na "/name")
    _snprintf_s(out, outSize, _TRUNCATE, "%s/%s", b, name);
    SftpNormalize(out);
}

void SftpParent(const char* path, char* out, int outSize)
{
    lstrcpyn(out, path, outSize);
    SftpNormalize(out);
    char* slash = strrchr(out, '/');
    if (slash == out)
        out[1] = 0; // parent rootu je root
    else if (slash != NULL)
        *slash = 0;
}

bool SftpIsSamePath(const char* a, const char* b)
{
    char na[MAX_PATH], nb[MAX_PATH];
    lstrcpyn(na, a, MAX_PATH);
    lstrcpyn(nb, b, MAX_PATH);
    SftpNormalize(na);
    SftpNormalize(nb);
    return strcmp(na, nb) == 0; // SFTP je case-sensitive
}

bool SftpIsRoot(const char* path)
{
    char n[MAX_PATH];
    lstrcpyn(n, path, MAX_PATH);
    SftpNormalize(n);
    return strcmp(n, "/") == 0;
}
