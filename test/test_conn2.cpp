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
    std::string uploadSource = LocalTestPath("chmodtest-source.txt");
    if (!WriteTestFile(uploadSource.c_str(), "SFTP chmod test fixture\n")) { printf("FAIL create local test fixture\n"); return 1; }

    // 1) Password authentication + new methods
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "connect with password");

        // ListDir s owner/group
        std::vector<CSftpEntry> entries;
        CHECK(c.ListDir("/", entries), "listdir with owner/group");
        printf("   sample: ");
        for (auto& e : entries) { printf("%s(o=%s,g=%s) ", e.Name.c_str(), e.Owner.c_str(), e.Group.c_str()); }
        printf("\n");

        // upload pro testy
        CHECK(c.Upload(LocalTestPath("chmodtest-source.txt").c_str(), "/chmodtest.txt"), "upload chmodtest");

        // chmod + getpermissions
        CHECK(c.Chmod("/chmodtest.txt", 0640), "chmod 640");
        unsigned long m = 0;
        CHECK(c.GetPermissions("/chmodtest.txt", m), "getpermissions");
        printf("   permissions after chmod: %03o (expected 640)\n", (unsigned)m);
        if ((m & 0777) != 0640) { printf("FAIL permissions mismatch\n"); }

        // StatFull
        unsigned __int64 sz; unsigned long perms, uid, gid, mtime;
        CHECK(c.StatFull("/chmodtest.txt", sz, perms, uid, gid, mtime), "statfull");
        printf("   statfull: size=%I64u perms=%03o\n", sz, (unsigned)perms);

        // ExecCommand
        std::string out;
        CHECK(c.ExecCommand("echo HELLO_FROM_SERVER", out), "execcommand");
        printf("   command output: %s", out.c_str());

        // GetSecurityInfo
        std::string sec;
        CHECK(c.GetSecurityInfo(sec), "getsecurityinfo");
        printf("   sec info:\n%s", sec.c_str());

        // cleanup
        CHECK(c.RemoveFile("/chmodtest.txt"), "delete chmodtest");
        c.Disconnect();
    }

    // 2) Key authentication
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "", LocalTestPath("sftp_testkey").c_str()), "connect with key");
        std::vector<CSftpEntry> e;
        CHECK(c.ListDir("/", e), "listdir with key");
        c.Disconnect();
    }

    CSftpConnection::GlobalExit();
    printf("\n=== ALL TESTS OK ===\n");
    return 0;
}
