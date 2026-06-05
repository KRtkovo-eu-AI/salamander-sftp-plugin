// Test UTF-8 konverze názvů (round-trip: zobrazení -> server -> zpět).
#include "sftpconn.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c,w) do{ if(!(c)){ printf("FAIL %s: %s\n",w,cn.LastError()); return 1;} printf("OK   %s\n",w);}while(0)

int main()
{
    if (!CSftpConnection::GlobalInit()) { printf("GlobalInit FAIL\n"); return 1; }
    CSftpConnection::SetEncoding(1); // UTF-8
    CSftpConnection cn;
    CHECK(cn.Connect("127.0.0.1", 2222, "test", "test"), "connect");

    // zobrazovaný (ANSI) název s diakritikou: 0xE9='é', 0xF8='ř'(CP1250)
    const char* disp = "/\xe9\xf8_utf.txt";
    CHECK(cn.Upload("D:\\weby\\sftp_plugin_dev\\test_utf8.cpp", disp), "upload pod ANSI nazev");

    // výpis: server vrací UTF-8, plugin převede zpět na ANSI -> musí sedět původní display
    std::vector<CSftpEntry> e;
    CHECK(cn.ListDir("/", e), "listdir");
    bool found = false;
    for (auto& x : e) if (x.Name == "\xe9\xf8_utf.txt") found = true;
    printf("   round-trip nazev nalezen: %s\n", found ? "ANO" : "NE");
    if (!found) { printf("FAIL round-trip nazvu nesedi\n"); for(auto&x:e) printf("     '%s'\n",x.Name.c_str()); return 1; }

    // download pres ANSI nazev (musí se převést na stejný UTF-8 a najít soubor)
    CHECK(cn.Download(disp, "D:\\weby\\sftp_plugin_dev\\utf_dl.txt"), "download pres ANSI nazev");
    CHECK(cn.RemoveFile(disp), "smazat");
    cn.Disconnect();
    CSftpConnection::GlobalExit();
    printf("\n=== UTF-8 TEST OK ===\n");
    return 0;
}
