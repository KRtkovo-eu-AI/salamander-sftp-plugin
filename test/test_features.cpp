// Test nových funkcí: host key (known_hosts), resume přenosu, .ppk klíč.
#include "sftpconn.h"
#include <stdio.h>
#include <string>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

static int g_hostKeyCalls = 0;
static int HostKeyCb(void*, const char* host, int port, const char* type, const char* fp, int status)
{
    g_hostKeyCalls++;
    printf("   [hostkey] %s:%d typ=%s status=%d fp=%.20s...\n", host, port, type, status, fp);
    return 2; // důvěřovat a uložit
}

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }

    const char* KH = "D:\\weby\\sftp_plugin_dev\\test_known_hosts";
    remove(KH);
    CSftpConnection::SetKnownHostsFile(KH);
    CSftpConnection::SetHostKeyCallback(HostKeyCb, nullptr);

    // --- 1) Host key: první připojení (neznámý -> uloží) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "1.connect (novy host key)");
        c.Disconnect();
        printf("   hostkey callback volan %dx (cekam 1)\n", g_hostKeyCalls);
        if (g_hostKeyCalls != 1) { printf("FAIL: callback se nevolal\n"); return 1; }
    }
    // --- 2) Druhé připojení: host key už uložen -> MATCH, bez callbacku ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "2.connect (znamy host key)");
        c.Disconnect();
        printf("   hostkey callback celkem %dx (cekam porad 1 = MATCH)\n", g_hostKeyCalls);
        if (g_hostKeyCalls != 1) { printf("FAIL: known_hosts MATCH nefunguje (callback %d)\n", g_hostKeyCalls); return 1; }
    }

    CSftpConnection::SetHostKeyCallback(nullptr, nullptr); // dál netřeba

    // --- 3) Resume přenosu (SFTP) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "3.connect");
        // nahraj zdroj
        CHECK(c.Upload("D:\\weby\\sftp_plugin_dev\\sftpconn.cpp", "/resume_src.bin"), "upload zdroj");
        unsigned __int64 rsize = 0;
        CHECK(c.RemoteFileSize("/resume_src.bin", rsize), "remotefilesize");
        printf("   velikost vzdaleneho: %I64u\n", rsize);

        // plné stažení = referenční
        CHECK(c.Download("/resume_src.bin", "D:\\weby\\sftp_plugin_dev\\ref.bin"), "plne stazeni (ref)");

        // vytvoř částečný soubor = první polovina ref.bin
        unsigned __int64 half = rsize / 2;
        {
            FILE* r = nullptr; fopen_s(&r, "D:\\weby\\sftp_plugin_dev\\ref.bin", "rb");
            FILE* p = nullptr; fopen_s(&p, "D:\\weby\\sftp_plugin_dev\\part.bin", "wb");
            char* b = new char[(size_t)half];
            fread(b, 1, (size_t)half, r);
            fwrite(b, 1, (size_t)half, p);
            delete[] b; fclose(r); fclose(p);
        }
        // resume od poloviny
        CHECK(c.Download("/resume_src.bin", "D:\\weby\\sftp_plugin_dev\\part.bin", half), "resume download od poloviny");

        // porovnej part.bin == ref.bin
        auto readall = [](const char* path, std::string& out)->bool{
            FILE* f=nullptr; fopen_s(&f,path,"rb"); if(!f) return false;
            char b[65536]; size_t n; while((n=fread(b,1,sizeof(b),f))>0) out.append(b,n); fclose(f); return true; };
        std::string a, b;
        readall("D:\\weby\\sftp_plugin_dev\\ref.bin", a);
        readall("D:\\weby\\sftp_plugin_dev\\part.bin", b);
        printf("   ref=%zu B, resumed=%zu B\n", a.size(), b.size());
        if (a != b) { printf("FAIL: resume data nesedi\n"); return 1; }
        printf("OK   resume data sedi (byte-exact)\n");
        c.RemoveFile("/resume_src.bin");
        c.Disconnect();
    }

    // --- 4) .ppk klíč (RSA, nešifrovaný) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "", "D:\\weby\\sftp_testkey.ppk"), "4.connect pres .ppk");
        std::vector<CSftpEntry> e;
        CHECK(c.ListDir("/", e), "listdir pres .ppk klic");
        printf("   .ppk: vypsano %d polozek\n", (int)e.size());
        c.Disconnect();
    }

    CSftpConnection::GlobalExit();
    printf("\n=== VSECHNY TESTY NOVYCH FUNKCI OK ===\n");
    return 0;
}
