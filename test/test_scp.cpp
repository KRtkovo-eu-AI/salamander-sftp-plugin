#include "sftpconn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

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

static bool WriteTestFile(const char* path, const char* content)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return true;
}

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }
    std::string uploadSource = LocalTestPath("scp-upload-source.txt");
    if (!WriteTestFile(uploadSource.c_str(), "SCP upload test fixture\n")) { printf("FAIL create local test fixture\n"); return 1; }

    CSftpConnection c;
    // protocol=1 (SCP), no compression, no fallback
    CHECK(c.Connect("127.0.0.1", 2223, "test", "test", nullptr, false, 1, false), "connect SCP");
    printf("   SCP mode: %s\n", c.IsScpMode() ? "YES" : "NO");

    // 1) ListDir through ls
    std::vector<CSftpEntry> entries;
    CHECK(c.ListDir("/", entries), "scp listdir");
    printf("   items: %d ->", (int)entries.size());
    for (auto& e : entries) printf(" %s%s", e.Name.c_str(), e.IsDir ? "/" : "");
    printf("\n");

    // 2) Upload through scp -t
    CHECK(c.Upload(LocalTestPath("scp-upload-source.txt").c_str(), "/scp_uploaded.txt"), "scp upload");

    // 3) PathType
    int t = c.PathType("/scp_uploaded.txt");
    printf("   file pathtype: %d (expected 1)\n", t);
    if (t != 1) { printf("FAIL pathtype\n"); return 1; }

    // 4) StatFull
    unsigned __int64 sz; unsigned long pr, uid, gid, mt;
    CHECK(c.StatFull("/scp_uploaded.txt", sz, pr, uid, gid, mt), "scp statfull");
    printf("   statfull: size=%I64u perms=%03o mtime=%lu\n", sz, (unsigned)pr, mt);

    // 5) Download through scp -f
    CHECK(c.Download("/scp_uploaded.txt", LocalTestPath("scp_dl.txt").c_str()), "scp download");

    // 6) verify downloaded size
    FILE* f = nullptr; fopen_s(&f, LocalTestPath("scp_dl.txt").c_str(), "rb");
    if (f) { _fseeki64(f, 0, SEEK_END); long long dl = _ftelli64(f); fclose(f);
             printf("   downloaded %lld B (source %I64u B)\n", dl, sz);
             if ((unsigned __int64)dl != sz) { printf("FAIL size mismatch\n"); return 1; } }

    // 7) MakeDir + Rename + RemoveDir
    CHECK(c.MakeDir("/scp_dir"), "scp mkdir");
    CHECK(c.Rename("/scp_dir", "/scp_dir2"), "scp rename");
    CHECK(c.RemoveDir("/scp_dir2"), "scp rmdir");

    // 8) Chmod (simulated on the Windows server) + cleanup
    CHECK(c.Chmod("/scp_uploaded.txt", 0640), "scp chmod");
    CHECK(c.RemoveFile("/scp_uploaded.txt"), "scp delete");

    c.Disconnect();
    CSftpConnection::GlobalExit();
    printf("\n=== ALL SCP TESTS OK ===\n");
    return 0;
}
