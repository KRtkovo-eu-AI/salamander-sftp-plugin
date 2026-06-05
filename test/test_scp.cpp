#include "sftpconn.h"
#include <stdio.h>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }

    CSftpConnection c;
    // protocol=1 (SCP), bez komprese, bez fallbacku
    CHECK(c.Connect("127.0.0.1", 2223, "test", "test", nullptr, false, 1, false), "connect SCP");
    printf("   SCP rezim: %s\n", c.IsScpMode() ? "ANO" : "NE");

    // 1) ListDir pres ls
    std::vector<CSftpEntry> entries;
    CHECK(c.ListDir("/", entries), "scp listdir");
    printf("   polozek: %d ->", (int)entries.size());
    for (auto& e : entries) printf(" %s%s", e.Name.c_str(), e.IsDir ? "/" : "");
    printf("\n");

    // 2) Upload pres scp -t
    CHECK(c.Upload("D:\\weby\\sftp_plugin_dev\\test_scp.cpp", "/scp_uploaded.txt"), "scp upload");

    // 3) PathType
    int t = c.PathType("/scp_uploaded.txt");
    printf("   pathtype souboru: %d (ocekavam 1)\n", t);
    if (t != 1) { printf("FAIL pathtype\n"); return 1; }

    // 4) StatFull
    unsigned __int64 sz; unsigned long pr, uid, gid, mt;
    CHECK(c.StatFull("/scp_uploaded.txt", sz, pr, uid, gid, mt), "scp statfull");
    printf("   statfull: size=%I64u perms=%03o mtime=%lu\n", sz, (unsigned)pr, mt);

    // 5) Download pres scp -f
    CHECK(c.Download("/scp_uploaded.txt", "D:\\weby\\sftp_plugin_dev\\scp_dl.txt"), "scp download");

    // 6) over velikost stazeneho
    FILE* f = nullptr; fopen_s(&f, "D:\\weby\\sftp_plugin_dev\\scp_dl.txt", "rb");
    if (f) { _fseeki64(f, 0, SEEK_END); long long dl = _ftelli64(f); fclose(f);
             printf("   stazeno %lld B (zdroj %I64u B)\n", dl, sz);
             if ((unsigned __int64)dl != sz) { printf("FAIL velikost nesedi\n"); return 1; } }

    // 7) MakeDir + Rename + RemoveDir
    CHECK(c.MakeDir("/scp_dir"), "scp mkdir");
    CHECK(c.Rename("/scp_dir", "/scp_dir2"), "scp rename");
    CHECK(c.RemoveDir("/scp_dir2"), "scp rmdir");

    // 8) Chmod (na Windows serveru predstirano) + uklid
    CHECK(c.Chmod("/scp_uploaded.txt", 0640), "scp chmod");
    CHECK(c.RemoveFile("/scp_uploaded.txt"), "scp delete");

    c.Disconnect();
    CSftpConnection::GlobalExit();
    printf("\n=== VSECHNY SCP TESTY OK ===\n");
    return 0;
}
