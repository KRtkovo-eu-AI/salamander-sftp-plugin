#include "sftpconn.h"
#include <stdio.h>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }

    // 1) Heslová autentizace + nové metody
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "test"), "connect heslem");

        // ListDir s owner/group
        std::vector<CSftpEntry> entries;
        CHECK(c.ListDir("/", entries), "listdir s owner/group");
        printf("   ukazka: ");
        for (auto& e : entries) { printf("%s(o=%s,g=%s) ", e.Name.c_str(), e.Owner.c_str(), e.Group.c_str()); }
        printf("\n");

        // upload pro testy
        CHECK(c.Upload("D:\\weby\\sftp_plugin_dev\\sftpconn.h", "/chmodtest.txt"), "upload chmodtest");

        // chmod + getpermissions
        CHECK(c.Chmod("/chmodtest.txt", 0640), "chmod 640");
        unsigned long m = 0;
        CHECK(c.GetPermissions("/chmodtest.txt", m), "getpermissions");
        printf("   prava po chmod: %03o (ocekavam 640)\n", (unsigned)m);
        if ((m & 0777) != 0640) { printf("FAIL prava nesedi\n"); }

        // StatFull
        unsigned __int64 sz; unsigned long perms, uid, gid, mtime;
        CHECK(c.StatFull("/chmodtest.txt", sz, perms, uid, gid, mtime), "statfull");
        printf("   statfull: size=%I64u perms=%03o\n", sz, (unsigned)perms);

        // ExecCommand
        std::string out;
        CHECK(c.ExecCommand("echo HELLO_FROM_SERVER", out), "execcommand");
        printf("   vystup prikazu: %s", out.c_str());

        // GetSecurityInfo
        std::string sec;
        CHECK(c.GetSecurityInfo(sec), "getsecurityinfo");
        printf("   sec info:\n%s", sec.c_str());

        // uklid
        CHECK(c.RemoveFile("/chmodtest.txt"), "delete chmodtest");
        c.Disconnect();
    }

    // 2) Klíčová autentizace
    {
        CSftpConnection c;
        CHECK(c.Connect("127.0.0.1", 2222, "test", "", "D:\\weby\\sftp_testkey"), "connect KLICEM");
        std::vector<CSftpEntry> e;
        CHECK(c.ListDir("/", e), "listdir pres klic");
        c.Disconnect();
    }

    CSftpConnection::GlobalExit();
    printf("\n=== VSECHNY TESTY OK ===\n");
    return 0;
}
