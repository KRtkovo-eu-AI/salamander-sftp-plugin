// Tests for host keys (known_hosts), transfer resume, and .ppk keys.
#include "sftpconn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

static int g_hostKeyCalls = 0;
static int HostKeyCb(void*, const char* host, int port, const char* type, const char* fp, int status)
{
    g_hostKeyCalls++;
    printf("   [hostkey] %s:%d type=%s status=%d fp=%.20s...\n", host, port, type, status, fp);
    return 2; // trust and save
}

static std::string LocalTestPath(const char* file)
{
    const char* root = getenv("SFTP_TEST_DATA_DIR");
    if (root == nullptr || root[0] == 0) root = getenv("TEMP");
    if (root == nullptr || root[0] == 0) root = getenv("TMP");
    if (root == nullptr || root[0] == 0) root = ".";
    std::string path(root);
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
    path += file;
    return path;
}

static bool WriteTestFile(const char* path, size_t size)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    for (size_t i = 0; i < size; ++i) { fputc((int)(i % 251), f); }
    fclose(f);
    return true;
}

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }
    std::string resumeSource = LocalTestPath("resume_src_local.bin");
    if (!WriteTestFile(resumeSource.c_str(), 128 * 1024)) { printf("FAIL create local resume fixture\n"); return 1; }

    std::string knownHosts = LocalTestPath("test_known_hosts");
    remove(knownHosts.c_str());
    CSftpConnection::SetKnownHostsFile(knownHosts.c_str());
    CSftpConnection::SetHostKeyCallback(HostKeyCb, nullptr);

    // --- 1) Host key: first connection (unknown -> saved) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "1.connect (new host key)");
        c.Disconnect();
        printf("   hostkey callback called %dx (expected 1)\n", g_hostKeyCalls);
        if (g_hostKeyCalls != 1) { printf("FAIL: callback was not called\n"); return 1; }
    }
    // --- 2) Second connection: host key already saved -> MATCH, no callback ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "2.connect (known host key)");
        c.Disconnect();
        printf("   hostkey callback total %dx (still expected 1 = MATCH)\n", g_hostKeyCalls);
        if (g_hostKeyCalls != 1) { printf("FAIL: known_hosts MATCH failed (callback %d)\n", g_hostKeyCalls); return 1; }
    }

    CSftpConnection::SetHostKeyCallback(nullptr, nullptr); // no longer needed

    // --- 3) Transfer resume (SFTP) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "3.connect");
        // upload source
        CHECK(c.Upload(LocalTestPath("resume_src_local.bin").c_str(), "/resume_src.bin"), "upload source");
        unsigned __int64 rsize = 0;
        CHECK(c.RemoteFileSize("/resume_src.bin", rsize), "remotefilesize");
        printf("   remote size: %I64u\n", rsize);

        // full download = reference
        CHECK(c.Download("/resume_src.bin", LocalTestPath("ref.bin").c_str()), "full download (ref)");

        // create partial file = first half of ref.bin
        unsigned __int64 half = rsize / 2;
        {
            FILE* r = nullptr; fopen_s(&r, LocalTestPath("ref.bin").c_str(), "rb");
            FILE* p = nullptr; fopen_s(&p, LocalTestPath("part.bin").c_str(), "wb");
            char* b = new char[(size_t)half];
            fread(b, 1, (size_t)half, r);
            fwrite(b, 1, (size_t)half, p);
            delete[] b; fclose(r); fclose(p);
        }
        // resume from the halfway point
        CHECK(c.Download("/resume_src.bin", LocalTestPath("part.bin").c_str(), half), "resume download from halfway");

        // compare part.bin == ref.bin
        auto readall = [](const char* path, std::string& out)->bool{
            FILE* f=nullptr; fopen_s(&f,path,"rb"); if(!f) return false;
            char b[65536]; size_t n; while((n=fread(b,1,sizeof(b),f))>0) out.append(b,n); fclose(f); return true; };
        std::string a, b;
        readall(LocalTestPath("ref.bin").c_str(), a);
        readall(LocalTestPath("part.bin").c_str(), b);
        printf("   ref=%zu B, resumed=%zu B\n", a.size(), b.size());
        if (a != b) { printf("FAIL: resume data mismatch\n"); return 1; }
        printf("OK   resume data matches (byte-exact)\n");
        c.RemoveFile("/resume_src.bin");
        c.Disconnect();
    }

    // --- 4) .ppk key (RSA, unencrypted) ---
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "", LocalTestPath("sftp_testkey.ppk").c_str()), "4.connect with .ppk");
        std::vector<CSftpEntry> e;
        CHECK(c.ListDir("/", e), "listdir with .ppk key");
        printf("   .ppk: listed %d items\n", (int)e.size());
        c.Disconnect();
    }

    CSftpConnection::GlobalExit();
    printf("\n=== ALL NEW FEATURE TESTS OK ===\n");
    return 0;
}
