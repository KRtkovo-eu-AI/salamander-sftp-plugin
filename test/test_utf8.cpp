// Test UTF-8 filename conversion (round-trip: display -> server -> back).
#include "sftpconn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define CHECK(c,w) do{ if(!(c)){ printf("FAIL %s: %s\n",w,cn.LastError()); return 1;} printf("OK   %s\n",w);}while(0)

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
    std::string uploadSource = LocalTestPath("utf8-upload-source.txt");
    if (!WriteTestFile(uploadSource.c_str(), "UTF-8 test fixture\n")) { printf("FAIL create local test fixture\n"); return 1; }

    CSftpConnection::SetEncoding(1); // UTF-8
    CSftpConnection cn;
    CHECK(cn.Connect("127.0.0.1", 2222, "test", "test"), "connect");

    // displayed (ANSI) name with diacritics: 0xE9='é', 0xF8='ř'(CP1250)
    const char* disp = "/\xe9\xf8_utf.txt";
    CHECK(cn.Upload(LocalTestPath("utf8-upload-source.txt").c_str(), disp), "upload with ANSI name");

    // listing: server returns UTF-8, plugin converts back to ANSI -> must match the original display name
    std::vector<CSftpEntry> e;
    CHECK(cn.ListDir("/", e), "listdir");
    bool found = false;
    for (auto& x : e) if (x.Name == "\xe9\xf8_utf.txt") found = true;
    printf("   round-trip name found: %s\n", found ? "YES" : "NO");
    if (!found) { printf("FAIL round-trip name mismatch\n"); for(auto&x:e) printf("     '%s'\n",x.Name.c_str()); return 1; }

    // download through ANSI name (must convert to the same UTF-8 and find the file)
    CHECK(cn.Download(disp, LocalTestPath("utf_dl.txt").c_str()), "download with ANSI name");
    CHECK(cn.RemoveFile(disp), "delete");
    cn.Disconnect();
    CSftpConnection::GlobalExit();
    printf("\n=== UTF-8 TEST OK ===\n");
    return 0;
}
