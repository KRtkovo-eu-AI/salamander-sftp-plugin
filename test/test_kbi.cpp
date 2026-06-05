// Test keyboard-interactive autentizace (server v KBI-only režimu na portu 2224).
#include "sftpconn.h"
#include <stdio.h>

#define CHECK(cond, what) do { if(!(cond)) { printf("FAIL %s: %s\n", what, c.LastError()); return 1; } printf("OK   %s\n", what); } while(0)

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }
    CSftpConnection c;
    // heslo "test" se má automaticky použít na první KBI výzvu (Heslo:)
    CHECK(c.Connect("127.0.0.1", 2224, "test", "test"), "connect keyboard-interactive");
    std::vector<CSftpEntry> e;
    CHECK(c.ListDir("/", e), "listdir pres KBI");
    printf("   KBI: vypsano %d polozek\n", (int)e.size());
    c.Disconnect();
    CSftpConnection::GlobalExit();
    printf("\n=== KBI TEST OK ===\n");
    return 0;
}
