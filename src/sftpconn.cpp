// Copyright © 2026 Dupl3xx
#include "sftpconn.h"
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/err.h>

CSftpConnection::ProgressFn CSftpConnection::Progress = nullptr;
void* CSftpConnection::ProgressCtx = nullptr;
CSftpConnection::HostKeyVerifyFn CSftpConnection::HostKeyCb = nullptr;
void* CSftpConnection::HostKeyCtx = nullptr;
CSftpConnection::KbdPromptFn CSftpConnection::KbdCb = nullptr;
void* CSftpConnection::KbdCtx = nullptr;
std::string CSftpConnection::KnownHostsFile;
int CSftpConnection::Encoding = 0;

void CSftpConnection::SetKnownHostsFile(const char* path)
{
    KnownHostsFile = (path != nullptr) ? path : "";
}

std::string CSftpConnection::ToServerEnc(const char* in)
{
    if (Encoding == 2 || in == nullptr || in[0] == 0)
        return (in != nullptr) ? in : "";
    wchar_t w[2 * MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, in, -1, w, 2 * MAX_PATH) == 0)
        return in;
    char out[4 * MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

std::string CSftpConnection::ToDisplayEnc(const char* in)
{
    if (Encoding == 2 || in == nullptr || in[0] == 0)
        return (in != nullptr) ? in : "";
    wchar_t w[2 * MAX_PATH];
    // jen platné UTF-8 převeď; jinak ponech (server může vracet i jiné kódování)
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, w, 2 * MAX_PATH) == 0)
        return in;
    char out[4 * MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

static std::string ShellQuote(const char* s); // definice níže (u SCP helperů)

bool CSftpConnection::ReportProgress(const char* name, unsigned __int64 done, unsigned __int64 total)
{
    if (Progress == nullptr)
        return true;
    return Progress(ProgressCtx, name, done, total);
}

CSftpConnection::CSftpConnection()
    : ScpMode(false), Sock(INVALID_SOCKET), Session(nullptr), Sftp(nullptr) {}

CSftpConnection::~CSftpConnection() { Disconnect(); }

// Načte libssh2.dll (a její OpenSSL závislosti) z adresáře tohoto pluginu.
// Bez toho by se kvůli pořadí hledání DLL mohla načíst cizí libssh2.dll z PATH
// (např. z PHP). Díky delay-loadu se libssh2 importy navážou až po tomto volání.
static void LoadBundledLibssh2()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&LoadBundledLibssh2, &self) ||
        self == nullptr)
        return;
    char path[MAX_PATH];
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0)
        return;
    char* slash = strrchr(path, '\\');
    if (slash == nullptr)
        return;
    // pořadí: nejdřív OpenSSL (závislost libssh2 i náš přímý import), pak libssh2
    const char* dlls[] = {"libcrypto-3-x64.dll", "libssh2.dll"};
    for (int i = 0; i < 2; i++)
    {
        strcpy(slash + 1, dlls[i]);
        LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
}

bool CSftpConnection::GlobalInit()
{
    LoadBundledLibssh2();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
    return libssh2_init(0) == 0;
}

void CSftpConnection::GlobalExit()
{
    libssh2_exit();
    WSACleanup();
}

void CSftpConnection::SetError(const char* ctx)
{
    char buf[512];
    if (Session)
    {
        char* msg = nullptr;
        int len = 0;
        libssh2_session_last_error(Session, &msg, &len, 0);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s: %s", ctx, msg ? msg : "(neznámá chyba)");
    }
    else
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", ctx);
    ErrorMsg = buf;
}

bool CSftpConnection::Connect(const char* host, int port, const char* user, const char* password, const char* keyFile,
                              bool useCompression, int protocol, bool scpFallback)
{
    Disconnect();
    ScpMode = false;

    struct addrinfo hints = {0}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    _snprintf_s(portstr, sizeof(portstr), _TRUNCATE, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
    {
        ErrorMsg = "Nelze přeložit jméno hostitele.";
        return false;
    }
    Sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (Sock == INVALID_SOCKET || connect(Sock, res->ai_addr, (int)res->ai_addrlen) != 0)
    {
        ErrorMsg = "Nelze navázat TCP spojení.";
        freeaddrinfo(res);
        Disconnect();
        return false;
    }
    freeaddrinfo(res);

    Session = libssh2_session_init();
    if (!Session) { ErrorMsg = "libssh2_session_init selhalo."; Disconnect(); return false; }
    libssh2_session_set_blocking(Session, 1);

    // volitelná zlib komprese přenosu (preferuj, ale povol i bez komprese)
    if (useCompression)
    {
        libssh2_session_method_pref(Session, LIBSSH2_METHOD_COMP_CS, "zlib@openssh.com,zlib,none");
        libssh2_session_method_pref(Session, LIBSSH2_METHOD_COMP_SC, "zlib@openssh.com,zlib,none");
    }

    if (libssh2_session_handshake(Session, Sock)) { SetError("SSH handshake"); Disconnect(); return false; }

    // ověření host key (known_hosts + dotaz uživatele)
    if (!VerifyHostKey(host, port)) { Disconnect(); return false; }

    // autentizace: klíč / heslo / keyboard-interactive
    if (!Authenticate(user, password, keyFile)) { Disconnect(); return false; }

    if (protocol == 1)
    {
        // explicitně SCP – nepoužívej SFTP subsystém
        ScpMode = true;
        return true;
    }
    Sftp = libssh2_sftp_init(Session);
    if (!Sftp)
    {
        if (scpFallback)
        {
            // server nemá SFTP subsystém → nouzově SCP
            ScpMode = true;
            return true;
        }
        SetError("SFTP inicializace");
        Disconnect();
        return false;
    }
    return true;
}

void CSftpConnection::Disconnect()
{
    ScpMode = false;
    if (Sftp) { libssh2_sftp_shutdown(Sftp); Sftp = nullptr; }
    if (Session)
    {
        libssh2_session_disconnect(Session, "Nashledanou");
        libssh2_session_free(Session);
        Session = nullptr;
    }
    if (Sock != INVALID_SOCKET) { closesocket(Sock); Sock = INVALID_SOCKET; }
}

// ===================== Ověření host key (known_hosts) =====================

// mapuje libssh2 typ klíče na bit pro known_hosts (LIBSSH2_KNOWNHOST_KEY_*)
static int HostKeyBit(int ktype)
{
    switch (ktype)
    {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    }
}

static const char* HostKeyTypeName(int ktype)
{
    switch (ktype)
    {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return "RSA";
    case LIBSSH2_HOSTKEY_TYPE_DSS: return "DSS";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ED25519";
#endif
    default: return "neznámý";
    }
}

bool CSftpConnection::VerifyHostKey(const char* host, int port)
{
    size_t klen = 0;
    int ktype = 0;
    const char* hk = libssh2_session_hostkey(Session, &klen, &ktype);
    if (hk == nullptr) { ErrorMsg = "Nelze získat host key serveru."; return false; }

    // SHA256 otisk pro zobrazení uživateli
    char fpstr[128];
    const unsigned char* fp = (const unsigned char*)libssh2_hostkey_hash(Session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (fp != nullptr)
    {
        char* p = fpstr;
        for (int i = 0; i < 32; i++)
            p += sprintf_s(p, 4, "%02x%s", fp[i], i < 31 ? ":" : "");
    }
    else
        lstrcpynA(fpstr, "(neznámý)", sizeof(fpstr));

    int keybit = HostKeyBit(ktype);
    int status = 0; // 0 = neznámý, 1 = změněný

    LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(Session);
    if (kh != nullptr && !KnownHostsFile.empty())
        libssh2_knownhost_readfile(kh, KnownHostsFile.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    struct libssh2_knownhost* khentry = nullptr;
    int check = LIBSSH2_KNOWNHOST_CHECK_NOTFOUND;
    if (kh != nullptr)
        check = libssh2_knownhost_checkp(kh, host, port, hk, klen,
                                         LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | keybit,
                                         &khentry);

    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH)
    {
        if (kh != nullptr) libssh2_knownhost_free(kh);
        return true; // server je známý a klíč sedí
    }
    status = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) ? 1 : 0;

    // zeptej se uživatele (bez callbacku: neznámého důvěřuj jednou, změněný odmítni)
    int decision;
    if (HostKeyCb != nullptr)
        decision = HostKeyCb(HostKeyCtx, host, port, HostKeyTypeName(ktype), fpstr, status);
    else
        decision = (status == 1) ? 0 : 1;

    if (decision == 0)
    {
        ErrorMsg = (status == 1)
                       ? "Host key serveru se ZMĚNIL – připojení odmítnuto (možný útok MITM)."
                       : "Host key serveru není důvěryhodný – připojení odmítnuto.";
        if (kh != nullptr) libssh2_knownhost_free(kh);
        return false;
    }

    if (decision == 2 && kh != nullptr) // důvěřovat a uložit
    {
        if (khentry != nullptr) // přepiš změněný záznam
            libssh2_knownhost_del(kh, khentry);
        libssh2_knownhost_addc(kh, host, nullptr, hk, klen, nullptr, 0,
                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | keybit, nullptr);
        if (!KnownHostsFile.empty())
            libssh2_knownhost_writefile(kh, KnownHostsFile.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }
    if (kh != nullptr) libssh2_knownhost_free(kh);
    return true;
}

// ===================== Autentizace (heslo / klíč / keyboard-interactive) =====================

// data předaná do KBI callbacku přes session abstract
struct CKbdData
{
    const char* password;
    bool used;
    CSftpConnection::KbdPromptFn cb;
    void* cbctx;
};

static void KbdInteractiveThunk(const char* /*name*/, int /*name_len*/,
                                const char* /*instruction*/, int /*instruction_len*/,
                                int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                                LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract)
{
    CKbdData* d = (abstract != nullptr) ? (CKbdData*)*abstract : nullptr;
    for (int i = 0; i < num_prompts; i++)
    {
        char buf[1024];
        buf[0] = 0;
        bool got = false;
        if (d != nullptr && d->password != nullptr && !d->used)
        {
            // první výzva = typicky heslo → použij uložené heslo
            lstrcpynA(buf, d->password, sizeof(buf));
            d->used = true;
            got = true;
        }
        else if (d != nullptr && d->cb != nullptr)
        {
            std::string pr((const char*)prompts[i].text, prompts[i].length);
            got = d->cb(d->cbctx, pr.c_str(), prompts[i].echo != 0, buf, sizeof(buf));
        }
        if (got)
        {
            size_t l = strlen(buf);
            responses[i].text = (char*)malloc(l + 1);
            if (responses[i].text != nullptr)
            {
                memcpy(responses[i].text, buf, l + 1);
                responses[i].length = (unsigned int)l;
            }
        }
        else
        {
            responses[i].text = nullptr;
            responses[i].length = 0;
        }
    }
}

bool CSftpConnection::Authenticate(const char* user, const char* password, const char* keyFile)
{
    unsigned ulen = (unsigned)strlen(user);
    char* authlist = libssh2_userauth_list(Session, user, ulen);
    bool hasPwd = authlist != nullptr && strstr(authlist, "password") != nullptr;
    bool hasKbd = authlist != nullptr && strstr(authlist, "keyboard-interactive") != nullptr;

    // 1) autentizace klíčem (vč. .ppk)
    if (keyFile != nullptr && keyFile[0] != 0)
    {
        int ppk = TryPpkAuth(user, keyFile, password);
        if (ppk == 1) return true;
        if (ppk == 0) return false; // .ppk rozpoznán, ale selhal (chyba je nastavená)
        // ppk == -1: není .ppk → standardní OpenSSH/PEM klíč
        if (libssh2_userauth_publickey_fromfile(Session, user, nullptr, keyFile,
                                                (password != nullptr && password[0] != 0) ? password : nullptr) == 0)
            return true;
        SetError("Autentizace klíčem");
        return false;
    }

    // 2) heslo
    if (authlist == nullptr || hasPwd)
    {
        if (libssh2_userauth_password(Session, user, password) == 0)
            return true;
    }

    // 3) keyboard-interactive (i jako fallback po neúspěšném hesle)
    if (hasKbd || authlist == nullptr)
    {
        CKbdData d;
        d.password = (password != nullptr && password[0] != 0) ? password : nullptr;
        d.used = false;
        d.cb = KbdCb;
        d.cbctx = KbdCtx;
        void** abstract = libssh2_session_abstract(Session);
        void* saved = (abstract != nullptr) ? *abstract : nullptr;
        if (abstract != nullptr) *abstract = &d;
        int rc = libssh2_userauth_keyboard_interactive(Session, user, &KbdInteractiveThunk);
        if (abstract != nullptr) *abstract = saved;
        if (rc == 0) return true;
    }

    SetError("Autentizace selhala");
    return false;
}

// ===================== PuTTY .ppk klíče =====================

// přečte SSH string/mpint z blobu (uint32 délka + data); posune offset
static bool SshReadField(const std::vector<unsigned char>& blob, size_t& off,
                         const unsigned char** data, unsigned& len)
{
    if (off + 4 > blob.size()) return false;
    len = ((unsigned)blob[off] << 24) | ((unsigned)blob[off + 1] << 16) |
          ((unsigned)blob[off + 2] << 8) | (unsigned)blob[off + 3];
    off += 4;
    if (off + len > blob.size()) return false;
    *data = blob.data() + off;
    off += len;
    return true;
}

// base64 dekódování (OpenSSL)
static std::vector<unsigned char> B64Decode(const std::string& s)
{
    std::vector<unsigned char> out(s.size());
    int n = EVP_DecodeBlock((unsigned char*)out.data(), (const unsigned char*)s.data(), (int)s.size());
    if (n < 0) return std::vector<unsigned char>();
    // EVP_DecodeBlock zarovnává na 3 bajty; odečti padding '='
    size_t pad = 0;
    if (!s.empty() && s[s.size() - 1] == '=') pad++;
    if (s.size() >= 2 && s[s.size() - 2] == '=') pad++;
    out.resize((size_t)n - pad);
    return out;
}

// derivace klíče pro zašifrovaný PPK v2 (AES-256-CBC, SHA1)
static void Ppk2DeriveKey(const std::string& passphrase, unsigned char outKey[32])
{
    for (int seq = 0; seq < 2; seq++)
    {
        SHA_CTX c;
        SHA1_Init(&c);
        unsigned char idx[4] = {0, 0, 0, (unsigned char)seq};
        SHA1_Update(&c, idx, 4);
        SHA1_Update(&c, passphrase.data(), passphrase.size());
        unsigned char dig[20];
        SHA1_Final(dig, &c);
        memcpy(outKey + seq * 20, dig, seq == 0 ? 20 : 12);
    }
}

// derivace klíče+IV pro PPK v3 (Argon2id) – vrací false pokud OpenSSL Argon2 nemá
static bool Ppk3DeriveKey(const std::string& passphrase, const std::vector<unsigned char>& salt,
                          unsigned mem, unsigned passes, unsigned par, unsigned char out[80])
{
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (kdf == nullptr) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) return false;
    OSSL_PARAM params[7];
    int i = 0;
    params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void*)passphrase.data(), passphrase.size());
    params[i++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void*)salt.data(), salt.size());
    params[i++] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ARGON2_MEMCOST, &mem);
    params[i++] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ITER, &passes);
    params[i++] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ARGON2_LANES, &par);
    unsigned threads = par;
    params[i++] = OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_THREADS, &threads);
    params[i] = OSSL_PARAM_construct_end();
    int rc = EVP_KDF_derive(ctx, out, 80, params);
    EVP_KDF_CTX_free(ctx);
    return rc > 0;
}

int CSftpConnection::TryPpkAuth(const char* user, const char* keyFile, const char* passphrase)
{
    // načti soubor
    FILE* f = nullptr;
    fopen_s(&f, keyFile, "rb");
    if (f == nullptr) return -1;
    std::string text;
    char rb[4096];
    size_t rn;
    while ((rn = fread(rb, 1, sizeof(rb), f)) > 0) text.append(rb, rn);
    fclose(f);

    if (text.compare(0, 20, "PuTTY-User-Key-File-") != 0)
        return -1; // není .ppk

    // jednoduchý parser řádkových hlaviček "Klic: hodnota" + víceřádkové base64 sekce
    int version = (text[20] == '3') ? 3 : 2;
    std::string algo, encryption, pubB64, privB64;
    std::string a2salt; unsigned a2mem = 0, a2pass = 0, a2par = 0;
    {
        size_t pos = 0;
        auto getline = [&](std::string& line) -> bool {
            if (pos >= text.size()) return false;
            size_t e = text.find('\n', pos);
            line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = (e == std::string::npos) ? text.size() : e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        };
        std::string line;
        while (getline(line))
        {
            size_t colon = line.find(": ");
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 2);
            if (key.compare(0, 20, "PuTTY-User-Key-File-") == 0) algo = val;
            else if (key == "Encryption") encryption = val;
            else if (key == "Argon2-Salt") a2salt = val;
            else if (key == "Argon2-Memory") a2mem = atoi(val.c_str());
            else if (key == "Argon2-Passes") a2pass = atoi(val.c_str());
            else if (key == "Argon2-Parallelism") a2par = atoi(val.c_str());
            else if (key == "Public-Lines" || key == "Private-Lines")
            {
                int n = atoi(val.c_str());
                std::string blob;
                for (int k = 0; k < n; k++) { std::string l2; if (getline(l2)) blob += l2; }
                if (key == "Public-Lines") pubB64 = blob; else privB64 = blob;
            }
        }
    }

    std::vector<unsigned char> pub = B64Decode(pubB64);
    std::vector<unsigned char> priv = B64Decode(privB64);
    if (pub.empty() || priv.empty()) { SetError("Neplatný .ppk soubor"); return 0; }

    bool encrypted = (encryption == "aes256-cbc");
    if (encrypted)
    {
        if (passphrase == nullptr || passphrase[0] == 0)
        {
            ErrorMsg = ".ppk klíč je zašifrovaný – zadejte heslo (frázi) klíče do pole Heslo.";
            return 0;
        }
        unsigned char keyiv[80];
        unsigned char* aeskey = keyiv;
        unsigned char iv[16];
        memset(iv, 0, sizeof(iv));
        if (version == 3)
        {
            std::vector<unsigned char> salt = B64Decode(a2salt);
            if (!Ppk3DeriveKey(passphrase, salt, a2mem, a2pass, a2par ? a2par : 1, keyiv))
            {
                ErrorMsg = "Zašifrovaný .ppk v3 (Argon2) – tato verze OpenSSL ho neumí. Převeďte klíč přes PuTTYgen na nešifrovaný nebo OpenSSH.";
                return 0;
            }
            aeskey = keyiv;
            memcpy(iv, keyiv + 32, 16);
        }
        else
        {
            Ppk2DeriveKey(passphrase, keyiv); // 32 bajtů klíč, IV nuly
            aeskey = keyiv;
        }
        // dešifruj priv (AES-256-CBC, bez paddingu)
        std::vector<unsigned char> dec(priv.size());
        EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
        int outl = 0, tmpl = 0;
        EVP_DecryptInit_ex(c, EVP_aes_256_cbc(), nullptr, aeskey, iv);
        EVP_CIPHER_CTX_set_padding(c, 0);
        EVP_DecryptUpdate(c, dec.data(), &outl, priv.data(), (int)priv.size());
        EVP_DecryptFinal_ex(c, dec.data() + outl, &tmpl);
        EVP_CIPHER_CTX_free(c);
        dec.resize(outl + tmpl);
        priv = dec;
    }

    // rozeber public blob: první pole = název algoritmu
    size_t po = 0;
    const unsigned char* d0;
    unsigned l0;
    if (!SshReadField(pub, po, &d0, l0)) { SetError("Neplatný .ppk (public)"); return 0; }
    std::string keyalg((const char*)d0, l0);

    EVP_PKEY* pkey = nullptr;
    RSA* rsaForPem = nullptr;
    if (keyalg == "ssh-rsa")
    {
        const unsigned char *eD, *nD;
        unsigned eL, nL;
        if (!SshReadField(pub, po, &eD, eL) || !SshReadField(pub, po, &nD, nL)) { SetError("Neplatný RSA .ppk"); return 0; }
        size_t pp = 0;
        const unsigned char *dD, *pD, *qD, *iD;
        unsigned dL, pL, qL, iL;
        // private blob: mpint d, p, q, iqmp
        if (!SshReadField(priv, pp, &dD, dL) || !SshReadField(priv, pp, &pD, pL) ||
            !SshReadField(priv, pp, &qD, qL) || !SshReadField(priv, pp, &iD, iL))
        { SetError("Neplatný RSA .ppk (private)"); return 0; }
        BIGNUM* bn_e = BN_bin2bn(eD, eL, nullptr);
        BIGNUM* bn_n = BN_bin2bn(nD, nL, nullptr);
        BIGNUM* bn_d = BN_bin2bn(dD, dL, nullptr);
        BIGNUM* bn_p = BN_bin2bn(pD, pL, nullptr);
        BIGNUM* bn_q = BN_bin2bn(qD, qL, nullptr);
        BIGNUM* bn_iqmp = BN_bin2bn(iD, iL, nullptr);
        // dopočítej CRT exponenty: dmp1 = d mod (p-1), dmq1 = d mod (q-1)
        BN_CTX* bnctx = BN_CTX_new();
        BIGNUM* p1 = BN_dup(bn_p); BN_sub_word(p1, 1);
        BIGNUM* q1 = BN_dup(bn_q); BN_sub_word(q1, 1);
        BIGNUM* dmp1 = BN_new(); BN_mod(dmp1, bn_d, p1, bnctx);
        BIGNUM* dmq1 = BN_new(); BN_mod(dmq1, bn_d, q1, bnctx);
        BN_free(p1); BN_free(q1); BN_CTX_free(bnctx);
        RSA* rsa = RSA_new();
        RSA_set0_key(rsa, bn_n, bn_e, bn_d);          // přebírá vlastnictví
        RSA_set0_factors(rsa, bn_p, bn_q);
        RSA_set0_crt_params(rsa, dmp1, dmq1, bn_iqmp);
        rsaForPem = rsa;
        pkey = EVP_PKEY_new();
        EVP_PKEY_assign_RSA(pkey, rsa);
    }
    else if (keyalg == "ssh-ed25519")
    {
        const unsigned char* pubKey;
        unsigned pubL;
        if (!SshReadField(pub, po, &pubKey, pubL)) { SetError("Neplatný ed25519 .ppk"); return 0; }
        size_t pp = 0;
        const unsigned char* privKey;
        unsigned privL;
        if (!SshReadField(priv, pp, &privKey, privL) || privL < 32) { SetError("Neplatný ed25519 .ppk (private)"); return 0; }
        pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, privKey, 32);
    }
    else
    {
        ErrorMsg = std::string("Nepodporovaný typ .ppk klíče: ") + keyalg + " (podporováno: ssh-rsa, ssh-ed25519).";
        return 0;
    }
    if (pkey == nullptr) { SetError("Nelze sestavit klíč z .ppk"); return 0; }

    // exportuj do PEM (PKCS#8) a předej libssh2
    BIO* bio = BIO_new(BIO_s_mem());
    int wrote = (rsaForPem != nullptr)
                    ? PEM_write_bio_RSAPrivateKey(bio, rsaForPem, nullptr, nullptr, 0, nullptr, nullptr)
                    : PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    if (!wrote)
    {
        char errbuf[256];
        unsigned long e = ERR_get_error();
        ERR_error_string_n(e, errbuf, sizeof(errbuf));
        char diag[400];
        sprintf_s(diag, "Nelze převést .ppk na PEM [bio=%p pkey=%p rsa=%p err=0x%lx %s]",
                  (void*)bio, (void*)pkey, (void*)rsaForPem, e, errbuf);
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        ErrorMsg = diag;
        return 0;
    }
    char* pem = nullptr;
    long pemlen = BIO_get_mem_data(bio, &pem);
    int rc = libssh2_userauth_publickey_frommemory(Session, user, strlen(user),
                                                   nullptr, 0, pem, pemlen, nullptr);
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    if (rc == 0) return 1; // úspěch
    SetError("Autentizace .ppk klíčem selhala");
    return 0;
}

bool CSftpConnection::RemoteFileSize(const char* remotePath, unsigned __int64& size)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    size = 0;
    if (ScpMode)
    {
        std::string cmd = "stat -c %s -- " + ShellQuote(remotePath);
        std::string out;
        int code = -1;
        if (!ExecRaw(cmd.c_str(), out, &code) || code != 0) return false;
        size = _strtoui64(out.c_str(), nullptr, 10);
        return true;
    }
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    if (libssh2_sftp_stat(Sftp, remotePath, &a) != 0) return false;
    if (a.flags & LIBSSH2_SFTP_ATTR_SIZE) size = a.filesize;
    return true;
}

// uvozovkování cesty pro shell (POSIX): obal do '...' a escapuj vnitřní '
static std::string ShellQuote(const char* s)
{
    std::string q = "'";
    for (const char* p = s; *p; p++)
    {
        if (*p == '\'')
            q += "'\\''";
        else
            q += *p;
    }
    q += "'";
    return q;
}

// převede rwx řetězec práv (např. "rwxr-x---") na osmičkový mód
static unsigned long RwxToMode(const char* rwx)
{
    unsigned long m = 0;
    for (int i = 0; i < 9 && rwx[i]; i++)
    {
        m <<= 1;
        char c = rwx[i];
        if (c != '-' && c != ' ')
            m |= 1; // r/w/x/s/t/S/T -> bit nastaven
    }
    return m;
}

bool CSftpConnection::ExecRaw(const char* command, std::string& out, int* exitCode)
{
    out.clear();
    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(Session);
    if (!ch) { SetError("Otevření kanálu"); return false; }
    if (libssh2_channel_exec(ch, command))
    {
        SetError("Spuštění příkazu");
        libssh2_channel_free(ch);
        return false;
    }
    char buf[8192];
    ssize_t n;
    while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0)
        out.append(buf, (size_t)n);
    while ((n = libssh2_channel_read_stderr(ch, buf, sizeof(buf))) > 0)
        out.append(buf, (size_t)n);
    libssh2_channel_close(ch);
    if (exitCode != nullptr)
        *exitCode = libssh2_channel_get_exit_status(ch);
    libssh2_channel_free(ch);
    return true;
}

bool CSftpConnection::ExecSimple(const char* command)
{
    std::string out;
    int code = -1;
    if (!ExecRaw(command, out, &code))
        return false;
    if (code != 0)
    {
        ErrorMsg = out.empty() ? "Příkaz na serveru selhal." : out;
        return false;
    }
    return true;
}

// SCP výpis adresáře přes "ls -la" (epoch čas → jedno pole, kvůli mezerám v názvech)
bool CSftpConnection::ScpListDir(const char* remotePath, std::vector<CSftpEntry>& out)
{
    out.clear();
    std::string cmd = "ls -la --time-style=+%s -- " + ShellQuote(remotePath);
    std::string text;
    int code = -1;
    if (!ExecRaw(cmd.c_str(), text, &code))
        return false;
    if (code != 0)
    {
        ErrorMsg = "Nelze vypsat adresář (SCP/ls).";
        return false;
    }
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.compare(0, 6, "total ") == 0)
            continue;
        // pole: perms links owner group size mtime name...
        char perms[16] = {0}, owner[128] = {0}, group[128] = {0};
        unsigned __int64 size = 0;
        unsigned long mtime = 0;
        int links = 0;
        // ručně rozeber prvních 6 polí, zbytek = název
        std::vector<std::string> f;
        {
            size_t i = 0;
            while (i < line.size() && f.size() < 6)
            {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
                size_t start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
                if (i > start) f.push_back(line.substr(start, i - start));
            }
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (f.size() < 6) continue; // nečekaný řádek
            std::string name = line.substr(i);
            lstrcpynA(perms, f[0].c_str(), sizeof(perms));
            links = atoi(f[1].c_str());
            lstrcpynA(owner, f[2].c_str(), sizeof(owner));
            lstrcpynA(group, f[3].c_str(), sizeof(group));
            size = _strtoui64(f[4].c_str(), nullptr, 10);
            mtime = (unsigned long)_strtoui64(f[5].c_str(), nullptr, 10);

            CSftpEntry e;
            e.IsDir = (perms[0] == 'd');
            e.IsLink = (perms[0] == 'l');
            if (e.IsLink)
            {
                // "name -> target" → ber jen název před " -> "
                size_t arrow = name.find(" -> ");
                if (arrow != std::string::npos)
                    name = name.substr(0, arrow);
                e.IsDir = false;
            }
            if (name == "." || name == "..")
                continue;
            e.Name = ToDisplayEnc(name.c_str());
            e.Size = size;
            e.MTime = mtime;
            e.Permissions = RwxToMode(perms + 1);
            e.Owner = owner;
            e.Group = group;
            out.push_back(e);
        }
        (void)links;
    }
    return true;
}

bool CSftpConnection::ScpDownload(const char* remotePath, const char* localPath)
{
    libssh2_struct_stat st;
    memset(&st, 0, sizeof(st));
    LIBSSH2_CHANNEL* ch = libssh2_scp_recv2(Session, remotePath, &st);
    if (!ch) { SetError("SCP stažení (otevření)"); return false; }
    FILE* f = nullptr;
    fopen_s(&f, localPath, "wb");
    if (!f) { ErrorMsg = "Nelze vytvořit lokální soubor."; libssh2_channel_free(ch); return false; }
    unsigned __int64 total = (unsigned __int64)st.st_size;
    unsigned __int64 done = 0;
    char buf[65536];
    bool ok = true;
    if (!ReportProgress(localPath, 0, total)) { ErrorMsg = "Přerušeno uživatelem."; fclose(f); libssh2_channel_free(ch); return false; }
    while (done < total)
    {
        size_t want = (size_t)((total - done) < sizeof(buf) ? (total - done) : sizeof(buf));
        ssize_t n = libssh2_channel_read(ch, buf, want);
        if (n < 0) { SetError("SCP čtení"); ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ErrorMsg = "Zápis na disk selhal."; ok = false; break; }
        done += (unsigned __int64)n;
        if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; ok = false; break; }
    }
    fclose(f);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    return ok;
}

bool CSftpConnection::ScpUpload(const char* localPath, const char* remotePath)
{
    FILE* f = nullptr;
    fopen_s(&f, localPath, "rb");
    if (!f) { ErrorMsg = "Nelze otevřít lokální soubor."; return false; }
    _fseeki64(f, 0, SEEK_END);
    unsigned __int64 total = (unsigned __int64)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    LIBSSH2_CHANNEL* ch = libssh2_scp_send64(Session, remotePath, 0644, (libssh2_int64_t)total, 0, 0);
    if (!ch) { SetError("SCP nahrání (otevření)"); fclose(f); return false; }
    char buf[65536];
    bool ok = true;
    unsigned __int64 done = 0;
    size_t n;
    if (!ReportProgress(localPath, 0, total)) { ErrorMsg = "Přerušeno uživatelem."; fclose(f); libssh2_channel_free(ch); return false; }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        char* p = buf;
        size_t left = n;
        while (left > 0)
        {
            ssize_t w = libssh2_channel_write(ch, p, left);
            if (w < 0) { SetError("SCP zápis"); ok = false; break; }
            p += w; left -= (size_t)w;
        }
        if (!ok) break;
        done += (unsigned __int64)n;
        if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; ok = false; break; }
    }
    fclose(f);
    if (ok)
    {
        libssh2_channel_send_eof(ch);
        libssh2_channel_wait_eof(ch);
        libssh2_channel_wait_closed(ch);
    }
    libssh2_channel_free(ch);
    return ok;
}

bool CSftpConnection::ListDir(const char* remotePath, std::vector<CSftpEntry>& out)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ScpListDir(remotePath, out);
    out.clear();
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(Sftp, remotePath);
    if (!dir) { SetError("Otevření adresáře"); return false; }
    char name[1024];
    char longentry[2048];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc;
    while ((rc = libssh2_sftp_readdir_ex(dir, name, sizeof(name), longentry, sizeof(longentry), &attrs)) > 0)
    {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        CSftpEntry e;
        e.Name = ToDisplayEnc(name);
        e.IsDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.IsLink = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
        e.Size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attrs.filesize : 0;
        e.MTime = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (unsigned long)attrs.mtime : 0;
        e.Permissions = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? attrs.permissions : 0;

        // vlastník/skupina: zkus z "ls -l" longentry (pole 2 a 3), jinak UID/GID
        e.Owner.clear();
        e.Group.clear();
        if (longentry[0] != 0)
        {
            char tmp[2048];
            lstrcpynA(tmp, longentry, sizeof(tmp));
            char* ctx = NULL;
            char* tok = strtok_s(tmp, " \t", &ctx);
            int field = 0;
            while (tok != NULL)
            {
                if (field == 2)
                    e.Owner = tok;
                else if (field == 3)
                {
                    e.Group = tok;
                    break;
                }
                field++;
                tok = strtok_s(NULL, " \t", &ctx);
            }
        }
        if (e.Owner.empty() && (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID))
        {
            char b[32];
            sprintf_s(b, "%lu", attrs.uid);
            e.Owner = b;
            sprintf_s(b, "%lu", attrs.gid);
            e.Group = b;
        }
        if (e.IsLink)
            e.IsDir = false; // symlink zobrazíme jako soubor (cíl neřešíme)
        out.push_back(e);
    }
    libssh2_sftp_closedir(dir);
    return true;
}

int CSftpConnection::PathType(const char* remotePath)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
    {
        std::string q = ShellQuote(remotePath);
        std::string cmd = "if [ -d " + q + " ]; then echo D; elif [ -e " + q + " ]; then echo F; else echo N; fi";
        std::string out;
        int code = -1;
        if (!ExecRaw(cmd.c_str(), out, &code))
            return 0;
        if (out.find('D') != std::string::npos) return 2;
        if (out.find('F') != std::string::npos) return 1;
        return 0;
    }
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_stat(Sftp, remotePath, &a) != 0)
        return 0; // neexistuje (nebo nedostupné)
    if ((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions))
        return 2;
    return 1;
}

bool CSftpConnection::Chmod(const char* remotePath, unsigned long mode)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
    {
        char oct[16];
        sprintf_s(oct, "%lo", mode & 0777);
        std::string cmd = "chmod " + std::string(oct) + " -- " + ShellQuote(remotePath);
        return ExecSimple(cmd.c_str());
    }
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    a.permissions = mode;
    if (libssh2_sftp_setstat(Sftp, remotePath, &a) != 0)
    {
        SetError("Změna práv");
        return false;
    }
    return true;
}

bool CSftpConnection::GetPermissions(const char* remotePath, unsigned long& mode)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
    {
        std::string cmd = "stat -c %a -- " + ShellQuote(remotePath);
        std::string out;
        int code = -1;
        if (!ExecRaw(cmd.c_str(), out, &code) || code != 0)
        {
            ErrorMsg = "Načtení práv (SCP/stat).";
            return false;
        }
        mode = strtoul(out.c_str(), nullptr, 8) & 0777;
        return true;
    }
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    if (libssh2_sftp_stat(Sftp, remotePath, &a) != 0)
    {
        SetError("Načtení práv");
        return false;
    }
    mode = a.permissions & 0777;
    return true;
}

bool CSftpConnection::StatFull(const char* remotePath, unsigned __int64& size, unsigned long& perms,
                               unsigned long& uid, unsigned long& gid, unsigned long& mtime)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
    {
        std::string cmd = "stat -c '%a %s %u %g %Y' -- " + ShellQuote(remotePath);
        std::string out;
        int code = -1;
        if (!ExecRaw(cmd.c_str(), out, &code) || code != 0)
        {
            ErrorMsg = "Načtení informací (SCP/stat).";
            return false;
        }
        unsigned long pmode = 0;
        if (sscanf_s(out.c_str(), "%lo %llu %lu %lu %lu", &pmode, &size, &uid, &gid, &mtime) < 5)
        {
            ErrorMsg = "Neočekávaný výstup stat.";
            return false;
        }
        perms = pmode & 0777;
        return true;
    }
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    if (libssh2_sftp_stat(Sftp, remotePath, &a) != 0)
    {
        SetError("Načtení informací");
        return false;
    }
    size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? a.filesize : 0;
    perms = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? (a.permissions & 0777) : 0;
    uid = (a.flags & LIBSSH2_SFTP_ATTR_UIDGID) ? a.uid : 0;
    gid = (a.flags & LIBSSH2_SFTP_ATTR_UIDGID) ? a.gid : 0;
    mtime = (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (unsigned long)a.mtime : 0;
    return true;
}

bool CSftpConnection::Download(const char* remotePath, const char* localPath, unsigned __int64 resumeOffset)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ScpDownload(remotePath, localPath); // SCP resume neumí
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(Sftp, remotePath, LIBSSH2_FXF_READ, 0);
    if (!h) { SetError("Otevření vzdáleného souboru"); return false; }
    LIBSSH2_SFTP_ATTRIBUTES at;
    unsigned __int64 total = 0;
    if (libssh2_sftp_fstat(h, &at) == 0 && (at.flags & LIBSSH2_SFTP_ATTR_SIZE))
        total = at.filesize;
    if (resumeOffset > 0 && resumeOffset <= total)
        libssh2_sftp_seek64(h, resumeOffset); // pokračuj od dané pozice
    else
        resumeOffset = 0;
    FILE* f = nullptr;
    fopen_s(&f, localPath, resumeOffset > 0 ? "r+b" : "wb");
    if (!f) { ErrorMsg = "Nelze otevřít lokální soubor."; libssh2_sftp_close(h); return false; }
    if (resumeOffset > 0) _fseeki64(f, (long long)resumeOffset, SEEK_SET);
    char buf[65536];
    bool ok = true;
    unsigned __int64 done = resumeOffset;
    if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; fclose(f); libssh2_sftp_close(h); return false; }
    for (;;)
    {
        ssize_t n = libssh2_sftp_read(h, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { SetError("Čtení vzdáleného souboru"); ok = false; break; }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ErrorMsg = "Zápis na disk selhal."; ok = false; break; }
        done += (unsigned __int64)n;
        if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; ok = false; break; }
    }
    fclose(f);
    libssh2_sftp_close(h);
    return ok;
}

bool CSftpConnection::Upload(const char* localPath, const char* remotePath, unsigned __int64 resumeOffset)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ScpUpload(localPath, remotePath); // SCP resume neumí
    FILE* f = nullptr;
    fopen_s(&f, localPath, "rb");
    if (!f) { ErrorMsg = "Nelze otevřít lokální soubor."; return false; }
    _fseeki64(f, 0, SEEK_END);
    unsigned __int64 total = (unsigned __int64)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    // resume: otevři bez TRUNC a naseek se na pozici; jinak přepiš od začátku
    long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;
    if (resumeOffset > 0 && resumeOffset < total)
        ; // ponech bez TRUNC
    else
    {
        flags |= LIBSSH2_FXF_TRUNC;
        resumeOffset = 0;
    }
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(Sftp, remotePath, flags,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!h) { SetError("Vytvoření vzdáleného souboru"); fclose(f); return false; }
    if (resumeOffset > 0)
    {
        libssh2_sftp_seek64(h, resumeOffset);
        _fseeki64(f, (long long)resumeOffset, SEEK_SET);
    }
    char buf[65536];
    bool ok = true;
    unsigned __int64 done = resumeOffset;
    size_t n;
    if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; fclose(f); libssh2_sftp_close(h); return false; }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        char* p = buf;
        size_t left = n;
        while (left > 0)
        {
            ssize_t w = libssh2_sftp_write(h, p, left);
            if (w < 0) { SetError("Zápis vzdáleného souboru"); ok = false; break; }
            p += w; left -= (size_t)w;
        }
        if (!ok) break;
        done += (unsigned __int64)n;
        if (!ReportProgress(localPath, done, total)) { ErrorMsg = "Přerušeno uživatelem."; ok = false; break; }
    }
    fclose(f);
    libssh2_sftp_close(h);
    return ok;
}

bool CSftpConnection::GetSecurityInfo(std::string& out)
{
    out.clear();
    if (!Session)
        return false;
    char line[256];
    // typ host key + SHA256 otisk
    int ktype = 0;
    size_t klen = 0;
    const char* hk = libssh2_session_hostkey(Session, &klen, &ktype);
    const char* ktypeName = "neznámý";
    if (ktype == LIBSSH2_HOSTKEY_TYPE_RSA) ktypeName = "RSA";
    else if (ktype == LIBSSH2_HOSTKEY_TYPE_DSS) ktypeName = "DSS";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    else if (ktype == LIBSSH2_HOSTKEY_TYPE_ECDSA_256 || ktype == LIBSSH2_HOSTKEY_TYPE_ECDSA_384 || ktype == LIBSSH2_HOSTKEY_TYPE_ECDSA_521) ktypeName = "ECDSA";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    else if (ktype == LIBSSH2_HOSTKEY_TYPE_ED25519) ktypeName = "ED25519";
#endif
    sprintf_s(line, "Typ host key: %s\r\n", ktypeName);
    out += line;

    const unsigned char* fp = (const unsigned char*)libssh2_hostkey_hash(Session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (fp)
    {
        out += "Otisk (SHA256): ";
        for (int i = 0; i < 32; i++)
        {
            sprintf_s(line, "%02x%s", fp[i], i < 31 ? ":" : "");
            out += line;
        }
        out += "\r\n";
    }
    const unsigned char* fp1 = (const unsigned char*)libssh2_hostkey_hash(Session, LIBSSH2_HOSTKEY_HASH_SHA1);
    if (fp1)
    {
        out += "Otisk (SHA1): ";
        for (int i = 0; i < 20; i++)
        {
            sprintf_s(line, "%02x%s", fp1[i], i < 19 ? ":" : "");
            out += line;
        }
        out += "\r\n";
    }
    const char* banner = libssh2_session_banner_get(Session);
    if (banner)
    {
        sprintf_s(line, "Banner serveru: %.200s\r\n", banner);
        out += line;
    }
    return true;
}

bool CSftpConnection::ExecCommand(const char* command, std::string& output)
{
    output.clear();
    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(Session);
    if (!ch) { SetError("Otevření kanálu"); return false; }
    if (libssh2_channel_exec(ch, command))
    {
        SetError("Spuštění příkazu");
        libssh2_channel_free(ch);
        return false;
    }
    char buf[8192];
    ssize_t n;
    while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0)
        output.append(buf, (size_t)n);
    // stderr
    while ((n = libssh2_channel_read_stderr(ch, buf, sizeof(buf))) > 0)
        output.append(buf, (size_t)n);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    return true;
}

bool CSftpConnection::MakeDir(const char* remotePath)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ExecSimple(("mkdir -- " + ShellQuote(remotePath)).c_str());
    if (libssh2_sftp_mkdir(Sftp, remotePath,
            LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH))
    { SetError("Vytvoření adresáře"); return false; }
    return true;
}

bool CSftpConnection::RemoveDir(const char* remotePath)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ExecSimple(("rmdir -- " + ShellQuote(remotePath)).c_str());
    if (libssh2_sftp_rmdir(Sftp, remotePath)) { SetError("Smazání adresáře"); return false; }
    return true;
}

bool CSftpConnection::RemoveFile(const char* remotePath)
{
    std::string _rp = ToServerEnc(remotePath);
    remotePath = _rp.c_str();
    if (ScpMode)
        return ExecSimple(("rm -f -- " + ShellQuote(remotePath)).c_str());
    if (libssh2_sftp_unlink(Sftp, remotePath)) { SetError("Smazání souboru"); return false; }
    return true;
}

bool CSftpConnection::Rename(const char* oldPath, const char* newPath)
{
    std::string _op = ToServerEnc(oldPath), _np = ToServerEnc(newPath);
    oldPath = _op.c_str();
    newPath = _np.c_str();
    if (ScpMode)
        return ExecSimple(("mv -- " + ShellQuote(oldPath) + " " + ShellQuote(newPath)).c_str());
    if (libssh2_sftp_rename(Sftp, oldPath, newPath)) { SetError("Přejmenování"); return false; }
    return true;
}
