// SPDX-FileCopyrightText: 2023 Open Salamander Authors
// SPDX-License-Identifier: GPL-2.0-or-later

//****************************************************************************
//
// Copyright (c) 2023 Open Salamander Authors
//
// This is a part of the Open Salamander SDK library.
//
//****************************************************************************

#include "precomp.h"
#include "sftpglue.h"

//
// ****************************************************************************
// CDeleteProgressDlg
//

CDeleteProgressDlg::CDeleteProgressDlg(HWND parent, CObjectOrigin origin)
    : CCommonDialog(HLanguage, IDD_PROGRESSDLG, parent, origin)
{
    ProgressBar = NULL;
    WantCancel = FALSE;
    LastTickCount = 0;
    TextCache[0] = 0;
    TextCacheIsDirty = FALSE;
    ProgressCache = 0;
    ProgressCacheIsDirty = FALSE;
}

void CDeleteProgressDlg::Set(const char* fileName, DWORD progress, BOOL dalayedPaint)
{
    lstrcpyn(TextCache, fileName != NULL ? fileName : "", MAX_PATH);
    TextCacheIsDirty = TRUE;

    if (progress != ProgressCache)
    {
        ProgressCache = progress;
        ProgressCacheIsDirty = TRUE;
    }

    if (!dalayedPaint)
        FlushDataToControls();
}

void CDeleteProgressDlg::EnableCancel(BOOL enable)
{
    if (HWindow != NULL)
    {
        HWND cancel = GetDlgItem(HWindow, IDCANCEL);
        if (IsWindowEnabled(cancel) != enable)
        {
            EnableWindow(cancel, enable);
            if (enable)
                SetFocus(cancel);
            PostMessage(cancel, BM_SETSTYLE, enable ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);

            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) // give the user a brief timeslice ...
            {
                if (!IsWindow(HWindow) || !IsDialogMessage(HWindow, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }
    }
}

BOOL CDeleteProgressDlg::GetWantCancel()
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, TRUE)) // give the user a brief moment ...
    {
        if (!IsWindow(HWindow) || !IsDialogMessage(HWindow, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // repaint changed data (text + progress bars) every 100 ms
    DWORD ticks = GetTickCount();
    if (ticks - LastTickCount > 100)
    {
        LastTickCount = ticks;
        FlushDataToControls();
    }

    return WantCancel;
}

void CDeleteProgressDlg::FlushDataToControls()
{
    if (HWindow != NULL)
    {
        if (TextCacheIsDirty)
        {
            SetDlgItemText(HWindow, IDT_FILENAME, TextCache);
            TextCacheIsDirty = FALSE;
        }

        if (ProgressCacheIsDirty)
        {
            ProgressBar->SetProgress(ProgressCache, NULL);
            ProgressCacheIsDirty = FALSE;
        }
    }
}

INT_PTR
CDeleteProgressDlg::DialogProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CALL_STACK_MESSAGE4("CPathDialog::DialogProc(0x%X, 0x%IX, 0x%IX)", uMsg, wParam, lParam);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        // use the Salamander-styled progress bar
        ProgressBar = SalamanderGUI->AttachProgressBar(HWindow, IDP_PROGRESSBAR);
        if (ProgressBar == NULL)
        {
            DestroyWindow(HWindow); // error -> do not show the dialog
            return FALSE;           // stop processing
        }

        break; // let DefDlgProc handle focus
    }

    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDCANCEL)
        {
            if (!WantCancel)
            {
                FlushDataToControls();

                if (SalamanderGeneral->SalMessageBox(HWindow, "Opravdu přerušit operaci?", "SFTP",
                                                     MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    WantCancel = TRUE;
                    EnableCancel(FALSE);
                }
            }
            return TRUE;
        }
        break;
    }
    }
    return CCommonDialog::DialogProc(uMsg, wParam, lParam);
}

//
// ****************************************************************************
// Progress přenosu (s rychlostí)
//

static CDeleteProgressDlg* g_ProgDlg = NULL;
static HWND g_ProgMainWnd = NULL;
static char g_ProgFile[MAX_PATH] = "";
static DWORD g_ProgStartTick = 0;

// stav přepisování souborů během přenosu
static HWND g_OvrParent = NULL;
static int g_OvrMode = 0;        // 0 = ptát se, 1 = přepsat vše, 2 = přeskočit vše
static bool g_OvrCancel = false; // uživatel zvolil Zrušit

// vrací: 1 = přepsat, 0 = přeskočit, -1 = zrušit celou operaci
static int SftpAskOverwrite(const char* targetName)
{
    if (g_OvrMode == 1)
        return 1;
    if (g_OvrMode == 2)
        return 0;
    const char* base = strrchr(targetName, '\\');
    const char* b2 = strrchr(targetName, '/');
    if (b2 > base)
        base = b2;
    base = (base != NULL) ? base + 1 : targetName;
    int r = SalamanderGeneral->DialogOverwrite(g_OvrParent != NULL ? g_OvrParent : g_ProgMainWnd,
                                               BUTTONS_YESALLSKIPCANCEL,
                                               base, "cílový soubor", base, "zdroj");
    switch (r)
    {
    case DIALOG_YES:
        return 1;
    case DIALOG_ALL:
        g_OvrMode = 1;
        return 1;
    case DIALOG_SKIP:
        return 0;
    case DIALOG_SKIPALL:
        g_OvrMode = 2;
        return 0;
    default:
        return -1; // Cancel
    }
}

// částečně existující cíl: 1 = navázat, 2 = přenést znovu, -1 = zrušit
static int SftpAskResume(const char* targetName, unsigned __int64 have, unsigned __int64 total)
{
    const char* base = strrchr(targetName, '\\');
    const char* b2 = strrchr(targetName, '/');
    if (b2 > base) base = b2;
    base = (base != NULL) ? base + 1 : targetName;
    char msg[600];
    _snprintf_s(msg, _TRUNCATE,
                "Soubor \"%s\" už částečně existuje (%I64u z %I64u bajtů).\n\n"
                "Navázat v přerušeném přenosu?\n\n"
                "Ano = navázat (pokračovat)\nNe = přenést celý znovu\nZrušit = přerušit operaci",
                base, have, total);
    int r = SalamanderGeneral->SalMessageBox(g_OvrParent != NULL ? g_OvrParent : g_ProgMainWnd, msg,
                                             "SFTP – navázat přenos", MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDYES ? 1 : (r == IDNO ? 2 : -1);
}

// velikost lokálního souboru (0 pokud neexistuje)
static unsigned __int64 LocalFileSize(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesEx(path, GetFileExInfoStandard, &fad))
        return 0;
    return ((unsigned __int64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static bool SftpProgressCallback(void* ctx, const char* name, unsigned __int64 done, unsigned __int64 total)
{
    if (g_ProgDlg == NULL)
        return true;
    // nový soubor -> reset měření rychlosti
    if (strcmp(g_ProgFile, name) != 0)
    {
        lstrcpyn(g_ProgFile, name, MAX_PATH);
        g_ProgStartTick = GetTickCount();
    }
    DWORD elapsed = GetTickCount() - g_ProgStartTick;
    double speedMB = elapsed > 0 ? ((double)done * 1000.0 / elapsed) / 1048576.0 : 0.0;
    const char* base = strrchr(name, '\\');
    const char* base2 = strrchr(name, '/');
    if (base2 > base)
        base = base2;
    base = (base != NULL) ? base + 1 : name;
    char txt[2 * MAX_PATH];
    if (total > 0)
        _snprintf_s(txt, _TRUNCATE, "%s   %I64u / %I64u kB   (%.2f MB/s)", base, done / 1024, total / 1024, speedMB);
    else
        _snprintf_s(txt, _TRUNCATE, "%s   %I64u kB   (%.2f MB/s)", base, done / 1024, speedMB);
    DWORD prog = total > 0 ? (DWORD)(done * 1000 / total) : 0;
    g_ProgDlg->Set(txt, prog, TRUE);
    return !g_ProgDlg->GetWantCancel();
}

static void SftpProgressBegin(HWND parent)
{
    g_ProgMainWnd = parent;
    HWND pw;
    while ((pw = GetParent(g_ProgMainWnd)) != NULL && IsWindowEnabled(pw))
        g_ProgMainWnd = pw;
    EnableWindow(g_ProgMainWnd, FALSE);
    g_ProgFile[0] = 0;
    g_ProgStartTick = GetTickCount();
    g_OvrMode = 0;
    g_OvrCancel = false;
    g_OvrParent = parent;
    g_ProgDlg = new CDeleteProgressDlg(g_ProgMainWnd, ooStatic);
    if (g_ProgDlg != NULL && g_ProgDlg->Create() != NULL)
    {
        SetForegroundWindow(g_ProgDlg->HWindow);
        g_ProgDlg->Set("Připojuji…", 0, FALSE);
        g_OvrParent = g_ProgDlg->HWindow;
        CSftpConnection::SetProgressCallback(SftpProgressCallback, NULL);
    }
    else
    {
        if (g_ProgDlg != NULL)
            delete g_ProgDlg;
        g_ProgDlg = NULL;
        EnableWindow(g_ProgMainWnd, TRUE);
    }
}

static void SftpProgressEnd()
{
    CSftpConnection::SetProgressCallback(NULL, NULL);
    if (g_ProgDlg != NULL)
    {
        EnableWindow(g_ProgMainWnd, TRUE);
        DestroyWindow(g_ProgDlg->HWindow);
        delete g_ProgDlg;
        g_ProgDlg = NULL;
    }
}

//
// ****************************************************************************
// CPluginFSInterface
//

CPluginFSInterface::CPluginFSInterface()
{
    Path[0] = 0;
    PathError = FALSE;
    FatalError = FALSE;
    CalledFromDisconnectDialog = FALSE;
}

void WINAPI
CPluginFSInterface::ReleaseObject(HWND parent)
{
    if (Path[0] != 0) // if the FS is initialized, remove our disk-cache copies when closing
    {
        // build a unique name for this FS root in the disk cache (covers all files from this FS)
        char uniqueFileName[2 * MAX_PATH];
        strcpy(uniqueFileName, AssignedFSName);
        strcat(uniqueFileName, ":");
        SalamanderGeneral->GetRootPath(uniqueFileName + strlen(uniqueFileName), Path);
        // filenames on disk are case-insensitive, the disk cache is case-sensitive, converting
        // to lowercase makes the disk cache behave case-insensitively as well
        SalamanderGeneral->ToLowerCase(uniqueFileName);
        SalamanderGeneral->RemoveFilesFromCache(uniqueFileName);
    }
}

BOOL WINAPI
CPluginFSInterface::GetRootPath(char* userPart)
{
    strcpy(userPart, "/"); // SFTP root je "/"
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::GetCurrentPath(char* userPart)
{
    strcpy(userPart, Path[0] != 0 ? Path : "/");
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::GetFullName(CFileData& file, int isDir, char* buf, int bufSize)
{
    if (isDir == 2) // up-dir
    {
        SftpParent(Path, buf, bufSize);
        return TRUE;
    }
    SftpJoin(Path, file.Name, buf, bufSize);
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::GetFullFSPath(HWND parent, const char* fsName, char* path, int pathSize, BOOL& success)
{
    // 'path' je relativní nebo absolutní user-part; spoj s aktuální cestou a předřaď "fsName:"
    char full[MAX_PATH];
    if (path[0] == '/')
        lstrcpyn(full, path, MAX_PATH);
    else
        SftpJoin(Path[0] != 0 ? Path : "/", path, full, MAX_PATH);
    SftpNormalize(full);
    success = (int)(strlen(full) + strlen(fsName) + 1) < pathSize;
    if (success)
        sprintf(path, "%s:%s", fsName, full);
    else
        SalamanderGeneral->SalMessageBox(parent, "Cesta je příliš dlouhá.", LoadStr(IDS_PLUGINNAME),
                                         MB_OK | MB_ICONEXCLAMATION);
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::IsCurrentPath(int currentFSNameIndex, int fsNameIndex, const char* userPart)
{
    return currentFSNameIndex == fsNameIndex && SftpIsSamePath(Path, userPart);
}

BOOL WINAPI
CPluginFSInterface::IsOurPath(int currentFSNameIndex, int fsNameIndex, const char* userPart)
{
    if (ConnectData.UseConnectData)
        return FALSE; // nové připojení z Connect dialogu
    // jedno připojení obsluhuje celý strom serveru
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::ChangePath(int currentFSNameIndex, char* fsName, int fsNameIndex,
                               const char* userPart, char* cutFileName, BOOL* pathWasCut,
                               BOOL forceRefresh, int mode)
{
    if (mode != 3 && (pathWasCut != NULL || cutFileName != NULL))
    {
        TRACE_E("Incorrect value of 'mode' in CPluginFSInterface::ChangePath().");
        mode = 3;
    }
    if (pathWasCut != NULL)
        *pathWasCut = FALSE;
    if (cutFileName != NULL)
        *cutFileName = 0;
    FatalError = FALSE;
    PathError = FALSE;

    HWND parent = SalamanderGeneral->GetMsgBoxParent();
    if (!SftpEnsureConnected(parent))
        return FALSE;

    // urči vstupní cestu
    char path[MAX_PATH];
    if (*userPart == 0 && ConnectData.UseConnectData) // data z Connect dialogu
        lstrcpyn(path, ConnectData.UserPart, MAX_PATH);
    else
        lstrcpyn(path, userPart, MAX_PATH);
    if (path[0] == 0)
        strcpy(path, "/");
    // relativní cestu naváž na aktuální
    if (path[0] != '/')
    {
        char joined[MAX_PATH];
        SftpJoin(Path[0] != 0 ? Path : "/", path, joined, MAX_PATH);
        lstrcpyn(path, joined, MAX_PATH);
    }
    SftpNormalize(path);

    BOOL fileNameAlreadyCut = FALSE;
    while (1)
    {
        int type = SftpConn.PathType(path); // 0=neexist,1=soubor,2=adresář
        if (type == 2)
        {
            lstrcpyn(Path, path, MAX_PATH);
            return TRUE;
        }
        // soubor nebo neexistuje -> zkrať poslední komponentu
        const char* slash = strrchr(path, '/');
        if (slash == NULL || slash == path) // už jsme na rootu a ten není adresář -> fatální
        {
            char msg[2 * MAX_PATH];
            _snprintf_s(msg, _TRUNCATE, "Cesta neexistuje nebo není adresář:\n%s:%s", fsName, userPart);
            SalamanderGeneral->SalMessageBox(parent, msg, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
        char lastComp[MAX_PATH];
        lstrcpyn(lastComp, slash + 1, MAX_PATH);
        char parentPath[MAX_PATH];
        SftpParent(path, parentPath, MAX_PATH);

        if (pathWasCut != NULL)
            *pathWasCut = TRUE;
        if (!fileNameAlreadyCut) // jen první odříznutí může být jméno souboru k zaostření
        {
            fileNameAlreadyCut = TRUE;
            if (cutFileName != NULL && type == 1)
                lstrcpyn(cutFileName, lastComp, MAX_PATH);
        }
        else if (cutFileName != NULL)
            *cutFileName = 0;

        lstrcpyn(path, parentPath, MAX_PATH);
    }
}

BOOL WINAPI
CPluginFSInterface::ListCurrentPath(CSalamanderDirectoryAbstract* dir,
                                    CPluginDataInterfaceAbstract*& pluginData,
                                    int& iconsType, BOOL forceRefresh)
{
    HWND parent = SalamanderGeneral->GetMsgBoxParent();
    if (!SftpEnsureConnected(parent))
    {
        PathError = TRUE;
        return FALSE;
    }

    std::vector<CSftpEntry> entries;
    if (!SftpConn.ListDir(Path[0] != 0 ? Path : "/", entries))
    {
        PathError = TRUE; // chyba výpisu -> ChangePath cestu zkrátí
        return FALSE;
    }

    pluginData = new CPluginFSDataInterface(Path);
    if (pluginData == NULL)
    {
        TRACE_E("Low memory");
        FatalError = TRUE;
        return FALSE;
    }
    iconsType = pitFromRegistry; // ikony podle přípony z registru (.pdf, .txt, …)

    dir->SetValidData(VALID_DATA_EXTENSION | VALID_DATA_SIZE | VALID_DATA_TYPE |
                      VALID_DATA_DATE | VALID_DATA_TIME | VALID_DATA_ATTRIBUTES |
                      VALID_DATA_HIDDEN);

    int sortByExtDirsAsFiles;
    SalamanderGeneral->GetConfigParameter(SALCFG_SORTBYEXTDIRSASFILES, &sortByExtDirsAsFiles,
                                          sizeof(sortByExtDirsAsFiles), NULL);

    // up-dir ".." (pokud nejsme v kořeni)
    if (!SftpIsRoot(Path))
    {
        CFileData up;
        memset(&up, 0, sizeof(up));
        up.Name = SalamanderGeneral->DupStr("..");
        up.NameLen = 2;
        up.Ext = up.Name + up.NameLen;
        up.Size = CQuadWord(0, 0);
        up.Attr = FILE_ATTRIBUTE_DIRECTORY;
        up.DosName = NULL;
        up.PluginData = (DWORD_PTR) new CFSData("", "", "");
        up.IconOverlayIndex = ICONOVERLAYINDEX_NOTUSED;
        if (up.Name == NULL || !dir->AddDir(NULL, up, pluginData))
        {
            if (up.Name != NULL)
                SalamanderGeneral->Free(up.Name);
            dir->Clear(pluginData);
            delete pluginData;
            FatalError = TRUE;
            return FALSE;
        }
    }

    for (size_t i = 0; i < entries.size(); i++)
    {
        CSftpEntry& e = entries[i];
        CFileData file;
        file.Name = SalamanderGeneral->DupStr(e.Name.c_str());
        if (file.Name == NULL)
        {
            TRACE_E("Low memory");
            dir->Clear(pluginData);
            delete pluginData;
            FatalError = TRUE;
            return FALSE;
        }
        file.NameLen = e.Name.length();
        if (!sortByExtDirsAsFiles && e.IsDir)
            file.Ext = file.Name + file.NameLen; // adresáře nemají příponu
        else
        {
            char* s = strrchr(file.Name, '.');
            file.Ext = (s != NULL) ? s + 1 : file.Name + file.NameLen;
        }
        file.Size = CQuadWord((DWORD)(e.Size & 0xFFFFFFFF), (DWORD)(e.Size >> 32));
        file.Attr = e.IsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        ULONGLONG ll = (ULONGLONG)e.MTime * 10000000ULL + 116444736000000000ULL; // unix -> FILETIME
        file.LastWrite.dwLowDateTime = (DWORD)ll;
        file.LastWrite.dwHighDateTime = (DWORD)(ll >> 32);
        file.DosName = NULL;
        file.Hidden = (!e.Name.empty() && e.Name[0] == '.') ? 1 : 0; // unixové skryté
        file.IsLink = e.IsLink ? 1 : 0;
        file.IsOffline = 0;
        file.IconOverlayIndex = ICONOVERLAYINDEX_NOTUSED;

        // sestavení řetězce práv (rwxr-xr-x)
        char rights[12];
        const char* pp = "rwxrwxrwx";
        rights[0] = e.IsLink ? 'l' : (e.IsDir ? 'd' : '-');
        for (int k = 0; k < 9; k++)
            rights[k + 1] = (e.Permissions & (1 << (8 - k))) ? pp[k] : '-';
        rights[10] = 0;
        CFSData* extData = new CFSData(rights, e.Owner.c_str(), e.Group.c_str());
        if (extData == NULL || !extData->IsGood())
        {
            if (extData != NULL)
                delete extData;
            SalamanderGeneral->Free(file.Name);
            dir->Clear(pluginData);
            delete pluginData;
            FatalError = TRUE;
            return FALSE;
        }
        file.PluginData = (DWORD_PTR)extData;

        BOOL ok = e.IsDir ? dir->AddDir(NULL, file, pluginData) : dir->AddFile(NULL, file, pluginData);
        if (!ok)
        {
            delete extData;
            SalamanderGeneral->Free(file.Name);
            dir->Clear(pluginData);
            delete pluginData;
            FatalError = TRUE;
            return FALSE;
        }
    }

    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::TryCloseOrDetach(BOOL forceClose, BOOL canDetach, BOOL& detach, int reason)
{
    if (CalledFromDisconnectDialog)
    {
        detach = FALSE; // we want to close the FS in any case
        return TRUE;
    }
    // bez ptaní: SFTP spojení při opuštění cesty prostě zavři (nedetachuj)
    detach = FALSE;
    return TRUE;
}

void WINAPI
CPluginFSInterface::Event(int event, DWORD param)
{
    char buf[MAX_PATH + 100];
    if (event == FSE_CLOSEORDETACHCANCELED)
    {
        sprintf(buf, "Close or detach of path \"%s\" was canceled (%s).", Path, (param == PANEL_LEFT ? "left" : "right"));
#ifdef SFTP_QUIET
        TRACE_I("Sftp: " << buf);
#else  // SFTP_QUIET
        SalamanderGeneral->ShowMessageBox(buf, "FS Event", MSGBOX_INFO);
#endif // SFTP_QUIET
    }

    if (event == FSE_OPENED)
    {
        sprintf(buf, "Path \"%s\" was opened in %s panel.", Path, (param == PANEL_LEFT ? "left" : "right"));
#ifdef SFTP_QUIET
        TRACE_I("Sftp: " << buf);
#else  // SFTP_QUIET
        SalamanderGeneral->ShowMessageBox(buf, "FS Event", MSGBOX_INFO);
#endif // SFTP_QUIET
    }

    if (event == FSE_DETACHED)
    {
        LastDetachedFS = this;

        sprintf(buf, "Path \"%s\" was detached (%s).", Path, (param == PANEL_LEFT ? "left" : "right"));
#ifdef SFTP_QUIET
        TRACE_I("Sftp: " << buf);
#else  // SFTP_QUIET
        SalamanderGeneral->ShowMessageBox(buf, "FS Event", MSGBOX_INFO);
#endif // SFTP_QUIET
    }

    if (event == FSE_ATTACHED)
    {
        if (this == LastDetachedFS)
            LastDetachedFS = NULL;

        sprintf(buf, "Path \"%s\" was attached (%s).", Path, (param == PANEL_LEFT ? "left" : "right"));
#ifdef SFTP_QUIET
        TRACE_I("Sftp: " << buf);
#else  // SFTP_QUIET
        SalamanderGeneral->ShowMessageBox(buf, "FS Event", MSGBOX_INFO);
#endif // SFTP_QUIET
    }

    if (event == FSE_ACTIVATEREFRESH) // the user activated Salamander (switched from another application)
    {
        // refresh the path;
        // we are inside CPluginFSInterface, so RefreshPanelPath cannot be used
        //    SalamanderGeneral->PostRefreshPanelPath((int)param);
        SalamanderGeneral->PostRefreshPanelFS(this);

        sprintf(buf, "Activate refresh on path \"%s\" (%s).", Path, (param == PANEL_LEFT ? "left" : "right"));
#ifdef SFTP_QUIET
        TRACE_I("Sftp: " << buf);
#else  // SFTP_QUIET
        SalamanderGeneral->ShowMessageBox(buf, "FS Event", MSGBOX_INFO);
#endif // SFTP_QUIET
    }

    /*  // simple test of receiving the "timer" event after the timer expires
  if (event == FSE_TIMER)
  {
    TRACE_I("CPluginFSInterface::Event(): timer event " << param);
    if (param == 1234)
    {
      SalamanderGeneral->AddPluginFSTimer(2000, this, 123456);
    }
    if (param == 123456)
    {
      SalamanderGeneral->AddPluginFSTimer(2000, this, 123452);
      SalamanderGeneral->AddPluginFSTimer(2000, this, 123452);
      SalamanderGeneral->AddPluginFSTimer(1500, this, 1234);
    }
  }
*/
}

DWORD WINAPI
CPluginFSInterface::GetSupportedServices()
{
    return FS_SERVICE_CONTEXTMENU |
           FS_SERVICE_SHOWPROPERTIES |
           FS_SERVICE_CHANGEATTRS |
           FS_SERVICE_COPYFROMDISKTOFS |
           FS_SERVICE_MOVEFROMDISKTOFS |
           FS_SERVICE_MOVEFROMFS |
           FS_SERVICE_COPYFROMFS |
           FS_SERVICE_DELETE |
           FS_SERVICE_VIEWFILE |
           FS_SERVICE_CREATEDIR |
           FS_SERVICE_ACCEPTSCHANGENOTIF |
           FS_SERVICE_QUICKRENAME |
           FS_SERVICE_COMMANDLINE |
           FS_SERVICE_SHOWINFO |
           FS_SERVICE_GETFREESPACE |
           FS_SERVICE_GETFSICON |
           FS_SERVICE_GETNEXTDIRLINEHOTPATH |
           FS_SERVICE_GETCHANGEDRIVEORDISCONNECTITEM |
           FS_SERVICE_SHOWSECURITYINFO |
           FS_SERVICE_GETPATHFORMAINWNDTITLE;
}

void WINAPI
CPluginFSInterface::ShowSecurityInfo(HWND parent)
{
    if (!SftpConn.IsConnected())
    {
        SalamanderGeneral->SalMessageBox(parent, "Není aktivní SFTP spojení.", LoadStr(IDS_PLUGINNAME),
                                         MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string info;
    SftpConn.GetSecurityInfo(info);
    SalamanderGeneral->SalMessageBox(parent, info.empty() ? "(bez informací)" : info.c_str(),
                                     "Bezpečnostní informace SFTP", MB_OK | MB_ICONINFORMATION);
}

BOOL WINAPI
CPluginFSInterface::GetChangeDriveOrDisconnectItem(const char* fsName, char*& title, HICON& icon, BOOL& destroyIcon)
{
    char txt[2 * MAX_PATH + 102];
    // the text will be the FS path (in Salamander format)
    txt[0] = '\t';
    strcpy(txt + 1, fsName);
    sprintf(txt + strlen(txt), ":%s\t", Path);
    // double any '&' characters so the path prints correctly
    SalamanderGeneral->DuplicateAmpersands(txt, 2 * MAX_PATH + 102);
    // append information about free space
    CQuadWord space;
    SalamanderGeneral->GetDiskFreeSpace(&space, Path, NULL);
    if (space != CQuadWord(-1, -1))
        SalamanderGeneral->PrintDiskSize(txt + strlen(txt), space, 0);
    title = SalamanderGeneral->DupStr(txt);
    if (title == NULL)
        return FALSE; // low-memory, no item will be shown

    SalamanderGeneral->GetRootPath(txt, Path);

    if (!SalamanderGeneral->GetFileIcon(txt, FALSE, &icon, SALICONSIZE_16, TRUE, TRUE))
        icon = NULL;
    // switched to our own implementation (lower memory use, working XOR icons)
    //SHFILEINFO shi;
    //if (SHGetFileInfo(txt, 0, &shi, sizeof(shi),
    //                  SHGFI_ICON | SHGFI_SMALLICON | SHGFI_SHELLICONSIZE))
    //{
    //  icon = shi.hIcon;  // icon successfully retrieved
    //}
    //else icon = NULL;  // no icon available
    destroyIcon = TRUE;
    return TRUE;
}

HICON WINAPI
CPluginFSInterface::GetFSIcon(BOOL& destroyIcon)
{
    char root[MAX_PATH];
    SalamanderGeneral->GetRootPath(root, Path);

    HICON icon;
    if (!SalamanderGeneral->GetFileIcon(root, FALSE, &icon, SALICONSIZE_16, TRUE, TRUE))
        icon = NULL;
    // switched to our own implementation (lower memory use, working XOR icons)
    //SHFILEINFO shi;
    //if (SHGetFileInfo(root, 0, &shi, sizeof(shi),
    //                  SHGFI_ICON | SHGFI_SMALLICON | SHGFI_SHELLICONSIZE))
    //{
    //  icon = shi.hIcon;  // icon successfully retrieved
    //}
    //else icon = NULL;  // no icon available (the standard one will be used)
    destroyIcon = TRUE;
    return icon;
}

void WINAPI
CPluginFSInterface::GetDropEffect(const char* srcFSPath, const char* tgtFSPath,
                                  DWORD allowedEffects, DWORD keyState, DWORD* dropEffect)
{                                                                                       // if Copy and Move are both available, choose Move when both FS instances share the same root
    if ((*dropEffect & DROPEFFECT_MOVE) && *dropEffect != DROPEFFECT_MOVE &&            // otherwise there is no point in checking
        SalamanderGeneral->StrNICmp(srcFSPath, AssignedFSName, AssignedFSNameLen) == 0) // only paths on our FS are relevant
    {
        const char* src = srcFSPath + AssignedFSNameLen + 1;
        const char* tgt = tgtFSPath + AssignedFSNameLen + 1;
        if (SalamanderGeneral->HasTheSameRootPath(src, tgt))
            *dropEffect = DROPEFFECT_MOVE;
    }
}

void WINAPI
CPluginFSInterface::GetFSFreeSpace(CQuadWord* retValue)
{
    if (Path[0] == 0)
        *retValue = CQuadWord(-1, -1);
    else
        SalamanderGeneral->GetDiskFreeSpace(retValue, Path, NULL);
}

BOOL WINAPI
CPluginFSInterface::GetNextDirectoryLineHotPath(const char* text, int pathLen, int& offset)
{
    const char* root = text; // pointer past the root portion of the path
    while (*root != 0 && *root != ':')
        root++;
    if (*root == ':')
    {
        root++;
        if (*root == '\\') // UNC path
        {
            root++;
            int c = 3;
            while (*root != 0)
            {
                if (*root == '\\' && --c == 0)
                    break;
                root++;
            }
        }
        else // standard path
        {
            int c = 3;
            while (*++root != 0 && --c)
                ;
        }
    }
    const char* s = text + offset;
    const char* end = text + pathLen;
    if (s >= end)
        return FALSE;
    if (s < root)
        offset = (int)(root - text);
    else
    {
        if (*s == '\\')
            s++;
        while (s < end && *s != '\\')
            s++;
        offset = (int)(s - text);
    }
    return s < end;
}

void WINAPI
CPluginFSInterface::ShowInfoDialog(const char* fsName, HWND parent)
{
    CQuadWord f;
    GetFSFreeSpace(&f);
    char num[100];
    if (f != CQuadWord(-1, -1))
        SalamanderGeneral->PrintDiskSize(num, f, 1);
    else
        strcpy(num, "(unknown)");

    char buf[1000];
    _snprintf_s(buf, _TRUNCATE, "SFTP připojení\n\nCesta: %s:%s", fsName, Path);
    SalamanderGeneral->SalMessageBox(parent, buf, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONINFORMATION);
}

BOOL WINAPI
CPluginFSInterface::ExecuteCommandLine(HWND parent, char* command, int& selFrom, int& selTo)
{
    if (command[0] == 0)
        return FALSE;
    if (!SftpEnsureConnected(parent))
        return TRUE;
    // spusť příkaz v aktuálním adresáři serveru
    char full[2 * MAX_PATH];
    _snprintf_s(full, _TRUNCATE, "cd \"%s\" && %s", Path[0] != 0 ? Path : "/", command);
    std::string output;
    if (!SftpConn.ExecCommand(full, output))
    {
        char eb[600];
        _snprintf_s(eb, _TRUNCATE, "Příkaz selhal:\n%s", SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
    }
    else
    {
        if (output.length() > 8000)
            output.resize(8000); // ořízni dlouhý výstup pro messagebox
        SalamanderGeneral->SalMessageBox(parent, output.empty() ? "(příkaz bez výstupu)" : output.c_str(),
                                         "Výstup příkazu", MB_OK | MB_ICONINFORMATION);
        SalamanderGeneral->PostRefreshPanelFS(this); // příkaz mohl změnit obsah
    }
    command[0] = 0; // vyčisti command line
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::QuickRename(const char* fsName, int mode, HWND parent, CFileData& file, BOOL isDir,
                                char* newName, BOOL& cancel)
{
    // if the plugin opens its own dialog, it should use CSalamanderGeneralAbstract::AlterFileName
    // ('format' according to SalamanderGeneral->GetConfigParameter(SALCFG_FILENAMEFORMAT))
    cancel = FALSE;
    if (mode == 1)
        return FALSE; // vyžádej standardní dialog

    char buf[2 * MAX_PATH];
    // syntaktická kontrola jména (bez lomítek)
    if (newName[0] == 0 || strchr(newName, '/') != NULL || strchr(newName, '\\') != NULL)
    {
        SalamanderGeneral->SalMessageBox(parent, "Neplatné jméno.", LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }

    // aplikuj masku
    SalamanderGeneral->MaskName(buf, 2 * MAX_PATH, file.Name, newName);
    lstrcpyn(newName, buf, MAX_PATH);

    char remoteFrom[MAX_PATH], remoteTo[MAX_PATH];
    SftpJoin(Path, file.Name, remoteFrom, MAX_PATH);
    SftpJoin(Path, newName, remoteTo, MAX_PATH);

    if (!SftpEnsureConnected(parent))
        return FALSE;
    if (!SftpConn.Rename(remoteFrom, remoteTo))
    {
        _snprintf_s(buf, _TRUNCATE, "Přejmenování selhalo:\n%s", SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, buf, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }
    SalamanderGeneral->PostChangeOnPathNotification(Path, isDir);
    return TRUE;
}

void WINAPI
CPluginFSInterface::AcceptChangeOnPathNotification(const char* fsName, const char* path, BOOL includingSubdirs)
{
    if (Path[0] == 0)
        return;
    // 'path' může být buď user-part ("/dir") nebo plná FS cesta ("fsName:/dir") -> odřízni prefix
    const char* userPart = path;
    int fsNameLen = (int)strlen(fsName);
    if (SalamanderGeneral->StrNICmp(path, fsName, fsNameLen) == 0 && path[fsNameLen] == ':')
        userPart = path + fsNameLen + 1; // naše FS cesta
    else if (path[0] != '/')
        return; // disková nebo cizí cesta -> nás se netýká

    // refresh panelu, když se změna týká naší aktuální cesty (nebo jejího podstromu)
    if (SftpIsSamePath(userPart, Path) ||
        (includingSubdirs && strncmp(Path, userPart, strlen(userPart)) == 0))
        SalamanderGeneral->PostRefreshPanelFS(this);
}

BOOL WINAPI
CPluginFSInterface::CreateDir(const char* fsName, int mode, HWND parent, char* newName, BOOL& cancel)
{
    cancel = FALSE;
    if (mode == 1)
        return FALSE; // vyžádej standardní dialog

    if (newName[0] == 0)
    {
        cancel = TRUE;
        return TRUE;
    }

    char remote[MAX_PATH];
    if (newName[0] == '/')
        lstrcpyn(remote, newName, MAX_PATH);
    else
        SftpJoin(Path, newName, remote, MAX_PATH);
    SftpNormalize(remote);

    if (!SftpEnsureConnected(parent))
        return FALSE;
    if (!SftpConn.MakeDir(remote))
    {
        char eb[600];
        _snprintf_s(eb, _TRUNCATE, "Nelze vytvořit adresář:\n%s", SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }
    SalamanderGeneral->PostChangeOnPathNotification(Path, FALSE);
    // zaostři nově vytvořený adresář (jen jeho jméno)
    const char* slash = strrchr(remote, '/');
    lstrcpyn(newName, slash != NULL ? slash + 1 : remote, MAX_PATH);
    return TRUE;
}

void WINAPI
CPluginFSInterface::ViewFile(const char* fsName, HWND parent,
                             CSalamanderForViewFileOnFSAbstract* salamander,
                             CFileData& file)
{
    // build a unique file name for the disk cache (standard Salamander path format)
    char uniqueFileName[2 * MAX_PATH];
    strcpy(uniqueFileName, fsName);
    strcat(uniqueFileName, ":");
    strcat(uniqueFileName, Path);
    SalamanderGeneral->SalPathAppend(uniqueFileName + strlen(fsName) + 1, file.Name, MAX_PATH);
    // filenames on disk are case-insensitive, the disk cache is case-sensitive, converting
    // to lowercase makes the disk cache behave case-insensitively as well
    SalamanderGeneral->ToLowerCase(uniqueFileName);

    // obtain the cache copy name
    BOOL fileExists;
    const char* tmpFileName = salamander->AllocFileNameInCache(parent, uniqueFileName, file.Name, NULL, fileExists);
    if (tmpFileName == NULL)
        return; // fatal error

    // je-li potřeba, stáhni soubor do disk cache přes SFTP
    BOOL newFileOK = FALSE;
    CQuadWord newFileSize(0, 0);
    if (!fileExists)
    {
        char remote[MAX_PATH];
        SftpJoin(Path, file.Name, remote, MAX_PATH);
        if (SftpEnsureConnected(parent) && SftpConn.Download(remote, tmpFileName))
        {
            newFileOK = TRUE;
            HANDLE hFile = HANDLES_Q(CreateFile(tmpFileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                NULL, OPEN_EXISTING, 0, NULL));
            if (hFile != INVALID_HANDLE_VALUE)
            {
                DWORD err;
                SalamanderGeneral->SalGetFileSize(hFile, newFileSize, err);
                HANDLES(CloseHandle(hFile));
            }
        }
        else
        {
            char errorText[3 * MAX_PATH + 100];
            _snprintf_s(errorText, _TRUNCATE, "Nelze stáhnout soubor %s.\n%s", file.Name, SftpConn.LastError());
            SalamanderGeneral->SalMessageBox(parent, errorText, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        }
    }

    // open the viewer
    HANDLE fileLock;
    BOOL fileLockOwner;
    if (!fileExists && !newFileOK || // open the viewer only if the file copy is OK
        !salamander->OpenViewer(parent, tmpFileName, &fileLock, &fileLockOwner))
    { // on failure reset the "lock"
        fileLock = NULL;
        fileLockOwner = FALSE;
    }

    // call FreeFileNameInCache to pair with AllocFileNameInCache (connects
    // the viewer and the disk cache)
    salamander->FreeFileNameInCache(uniqueFileName, fileExists, newFileOK,
                                    newFileSize, fileLock, fileLockOwner, FALSE /* do not delete immediately after closing the viewer */);
}

// rekurzivní smazání souboru/adresáře na SFTP
static bool SftpDeleteRecursive(const char* remote, bool isDir)
{
    if (!isDir)
        return SftpConn.RemoveFile(remote);
    std::vector<CSftpEntry> entries;
    if (SftpConn.ListDir(remote, entries))
    {
        for (size_t i = 0; i < entries.size(); i++)
        {
            char child[MAX_PATH];
            SftpJoin(remote, entries[i].Name.c_str(), child, MAX_PATH);
            if (!SftpDeleteRecursive(child, entries[i].IsDir))
                return false;
        }
    }
    return SftpConn.RemoveDir(remote);
}

// rekurzivní stažení souboru/adresáře z SFTP na disk
static bool SftpDownloadRecursive(const char* remote, const char* local, bool isDir)
{
    if (!isDir)
    {
        if (GetFileAttributes(local) != INVALID_FILE_ATTRIBUTES) // lokální soubor už existuje
        {
            unsigned __int64 localSize = LocalFileSize(local);
            unsigned __int64 remoteSize = 0;
            // nabídni navázání, když je lokální menší než vzdálený (jen SFTP)
            if (!SftpConn.IsScpMode() && localSize > 0 &&
                SftpConn.RemoteFileSize(remote, remoteSize) && remoteSize > localSize)
            {
                int r = SftpAskResume(local, localSize, remoteSize);
                if (r == -1) { g_OvrCancel = true; return false; }
                if (r == 1) return SftpConn.Download(remote, local, localSize); // navázat
                // r == 2 → přepsat (pokračuj plným stažením)
            }
            else
            {
                int a = SftpAskOverwrite(local);
                if (a == 0)
                    return true; // přeskočit (= úspěch)
                if (a < 0)
                {
                    g_OvrCancel = true;
                    return false;
                }
            }
        }
        return SftpConn.Download(remote, local);
    }
    CreateDirectory(local, NULL);
    std::vector<CSftpEntry> entries;
    if (!SftpConn.ListDir(remote, entries))
        return false;
    for (size_t i = 0; i < entries.size(); i++)
    {
        char r[MAX_PATH], l[2 * MAX_PATH];
        SftpJoin(remote, entries[i].Name.c_str(), r, MAX_PATH);
        lstrcpyn(l, local, 2 * MAX_PATH);
        SalamanderGeneral->SalPathAppend(l, entries[i].Name.c_str(), 2 * MAX_PATH);
        if (!SftpDownloadRecursive(r, l, entries[i].IsDir))
            return false;
    }
    return true;
}

// rekurzivní nahrání souboru/adresáře z disku na SFTP
static bool SftpUploadRecursive(const char* local, const char* remote, bool isDir)
{
    if (!isDir)
    {
        if (SftpConn.PathType(remote) == 1) // vzdálený soubor už existuje
        {
            unsigned __int64 localSize = LocalFileSize(local);
            unsigned __int64 remoteSize = 0;
            // nabídni navázání, když je vzdálený menší než lokální (jen SFTP)
            if (!SftpConn.IsScpMode() &&
                SftpConn.RemoteFileSize(remote, remoteSize) && remoteSize > 0 && remoteSize < localSize)
            {
                int r = SftpAskResume(remote, remoteSize, localSize);
                if (r == -1) { g_OvrCancel = true; return false; }
                if (r == 1) return SftpConn.Upload(local, remote, remoteSize); // navázat
                // r == 2 → přepsat
            }
            else
            {
                int a = SftpAskOverwrite(remote);
                if (a == 0)
                    return true; // přeskočit
                if (a < 0)
                {
                    g_OvrCancel = true;
                    return false;
                }
            }
        }
        return SftpConn.Upload(local, remote);
    }
    SftpConn.MakeDir(remote); // ignoruj chybu (může už existovat)
    char mask[2 * MAX_PATH];
    lstrcpyn(mask, local, 2 * MAX_PATH);
    SalamanderGeneral->SalPathAppend(mask, "*", 2 * MAX_PATH);
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(mask, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return true;
    bool ok = true;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char l[2 * MAX_PATH], r[MAX_PATH];
        lstrcpyn(l, local, 2 * MAX_PATH);
        SalamanderGeneral->SalPathAppend(l, fd.cFileName, 2 * MAX_PATH);
        SftpJoin(remote, fd.cFileName, r, MAX_PATH);
        bool childDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!SftpUploadRecursive(l, r, childDir))
        {
            ok = false;
            break;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
    return ok;
}

// rekurzivní smazání lokálního souboru/adresáře (pro operaci Move z disku)
static void LocalDeleteRecursive(const char* path, bool isDir)
{
    if (!isDir)
    {
        DeleteFile(path);
        return;
    }
    char mask[2 * MAX_PATH];
    lstrcpyn(mask, path, 2 * MAX_PATH);
    SalamanderGeneral->SalPathAppend(mask, "*", 2 * MAX_PATH);
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(mask, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char c[2 * MAX_PATH];
            lstrcpyn(c, path, 2 * MAX_PATH);
            SalamanderGeneral->SalPathAppend(c, fd.cFileName, 2 * MAX_PATH);
            LocalDeleteRecursive(c, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    RemoveDirectory(path);
}

// rekurzivní součet velikosti vzdáleného adresáře
static unsigned __int64 SftpDirSize(const char* remote, int& files, int& dirs)
{
    unsigned __int64 total = 0;
    std::vector<CSftpEntry> entries;
    if (!SftpConn.ListDir(remote, entries))
        return 0;
    for (size_t i = 0; i < entries.size(); i++)
    {
        char child[MAX_PATH];
        SftpJoin(remote, entries[i].Name.c_str(), child, MAX_PATH);
        if (entries[i].IsDir)
        {
            dirs++;
            total += SftpDirSize(child, files, dirs);
        }
        else
        {
            files++;
            total += entries[i].Size;
        }
    }
    return total;
}

// Upravit soubor: stáhni do temp, otevři v editoru, po potvrzení nahraj zpět.
void SftpEditFile(HWND parent, const char* remoteDir, const char* fileName)
{
    if (!SftpEnsureConnected(parent))
        return;
    char remote[MAX_PATH];
    SftpJoin(remoteDir, fileName, remote, MAX_PATH);
    char tmpDir[MAX_PATH], tmpFile[2 * MAX_PATH];
    GetTempPath(MAX_PATH, tmpDir);
    SalamanderGeneral->SalPathAppend(tmpDir, "SFTP_edit", MAX_PATH);
    CreateDirectory(tmpDir, NULL);
    lstrcpyn(tmpFile, tmpDir, 2 * MAX_PATH);
    SalamanderGeneral->SalPathAppend(tmpFile, fileName, 2 * MAX_PATH);

    if (!SftpConn.Download(remote, tmpFile))
    {
        char eb[600];
        _snprintf_s(eb, _TRUNCATE, "Nelze stáhnout soubor:\n%s", SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    ShellExecute(parent, "open", tmpFile, NULL, tmpDir, SW_SHOWNORMAL);
    if (SalamanderGeneral->SalMessageBox(parent,
                                         "Soubor byl otevřen v editoru.\n\nAž úpravy uložíte, klikněte na Ano pro nahrání zpět na server.\n(Ne = změny zahodit)",
                                         "Upravit soubor", MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
        if (SftpConn.Upload(tmpFile, remote))
        {
            char fsfull[MAX_PATH + 32];
            _snprintf_s(fsfull, _TRUNCATE, "%s:%s", AssignedFSName, remoteDir);
            SalamanderGeneral->PostChangeOnPathNotification(fsfull, FALSE);
        }
        else
        {
            char eb[600];
            _snprintf_s(eb, _TRUNCATE, "Nahrání selhalo:\n%s", SftpConn.LastError());
            SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        }
    }
    DeleteFile(tmpFile);
}

// Spočítat velikost vybraných položek na serveru.
void SftpCalcSize(HWND parent, const char* remoteDir, int panel)
{
    if (!SftpEnsureConnected(parent))
        return;
    int index = 0;
    BOOL isDir = FALSE;
    const CFileData* f;
    BOOL focused = (SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir) == NULL); // nic vybráno -> zaostřená položka
    index = 0;
    unsigned __int64 total = 0;
    int files = 0, dirs = 0;
    HCURSOR oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    while (1)
    {
        f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                    : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
        if (f == NULL)
            break;
        char remote[MAX_PATH];
        SftpJoin(remoteDir, f->Name, remote, MAX_PATH);
        if (isDir)
        {
            dirs++;
            total += SftpDirSize(remote, files, dirs);
        }
        else
        {
            files++;
            total += f->Size.Value;
        }
        if (focused)
            break;
    }
    SetCursor(oldCur);
    char info[400];
    _snprintf_s(info, _TRUNCATE,
                "Velikost: %I64u bajtů (%.2f MB)\nSouborů: %d\nAdresářů: %d",
                total, total / 1048576.0, files, dirs);
    SalamanderGeneral->SalMessageBox(parent, info, "Velikost na serveru", MB_OK | MB_ICONINFORMATION);
}

BOOL WINAPI
CPluginFSInterface::Delete(const char* fsName, int mode, HWND parent, int panel,
                           int selectedFiles, int selectedDirs, BOOL& cancelOrError)
{
    cancelOrError = FALSE;
    if (mode == 1)
        return FALSE; // vyžádej standardní potvrzení smazání

    if (!SftpEnsureConnected(parent))
    {
        cancelOrError = TRUE;
        return FALSE;
    }

    BOOL focused = (selectedFiles == 0 && selectedDirs == 0);
    int index = 0;
    BOOL isDir = FALSE;
    BOOL success = TRUE;
    const CFileData* f;
    while (1)
    {
        if (focused)
            f = SalamanderGeneral->GetPanelFocusedItem(panel, &isDir);
        else
            f = SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
        if (f == NULL)
            break;

        char remote[MAX_PATH];
        SftpJoin(Path, f->Name, remote, MAX_PATH);
        if (!SftpDeleteRecursive(remote, isDir != 0))
        {
            char eb[700];
            _snprintf_s(eb, _TRUNCATE, "Nelze smazat \"%s\":\n%s\n\nPokračovat dalšími položkami?",
                        f->Name, SftpConn.LastError());
            if (SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME),
                                                 MB_YESNO | MB_ICONEXCLAMATION) == IDNO)
            {
                success = FALSE;
                break;
            }
        }
        if (focused)
            break;
    }
    SalamanderGeneral->PostChangeOnPathNotification(Path, TRUE);
    cancelOrError = !success;
    return success;

    /*
  // fetch the "Confirm on" configuration values
  BOOL ConfirmOnNotEmptyDirDelete, ConfirmOnSystemHiddenFileDelete, ConfirmOnSystemHiddenDirDelete;
  SalamanderGeneral->GetConfigParameter(SALCFG_CNFRMNEDIRDEL, &ConfirmOnNotEmptyDirDelete, 4, NULL);
  SalamanderGeneral->GetConfigParameter(SALCFG_CNFRMSHFILEDEL, &ConfirmOnSystemHiddenFileDelete, 4, NULL);
  SalamanderGeneral->GetConfigParameter(SALCFG_CNFRMSHDIRDEL, &ConfirmOnSystemHiddenDirDelete, 4, NULL);

  char buf[2 * MAX_PATH];  // buffer for error texts

  char fileName[MAX_PATH];   // buffer for the full name
  strcpy(fileName, Path);
  char *end = fileName + strlen(fileName);  // space reserved for names from the panel
  if (end > fileName && *(end - 1) != '\\')
  {
    *end++ = '\\';
    *end = 0;
  }
  int endSize = MAX_PATH - (end - fileName);  // maximum number of characters available for a panel name

  char dfsFileName[2 * MAX_PATH];   // buffer for the full DFS name
  sprintf(dfsFileName, "%s:%s", fsName, fileName);
  char *endDFSName = dfsFileName + strlen(dfsFileName);  // space reserved for names from the panel
  int endDFSNameSize = 2 * MAX_PATH - (endDFSName - dfsFileName); // maximum number of characters available for a panel name

  const CFileData *f = NULL;  // pointer to the file/directory in the panel to process
  BOOL isDir = FALSE;         // TRUE if 'f' is a directory
  BOOL focused = (selectedFiles == 0 && selectedDirs == 0);
  int index = 0;
  BOOL success = TRUE;        // FALSE if an error occurs or the user cancels
  BOOL skipAllSHFD = FALSE;   // skip all deletes of system or hidden files
  BOOL yesAllSHFD = FALSE;    // delete all system or hidden files
  BOOL skipAllSHDD = FALSE;   // skip all deletes of system or hidden dirs
  BOOL yesAllSHDD = FALSE;    // delete all system or hidden dirs
  BOOL skipAllErrors = FALSE; // skip all errors
  BOOL changeInSubdirs = FALSE;
  while (1)
  {
    // fetch data for the file being processed
    if (focused) f = SalamanderGeneral->GetPanelFocusedItem(panel, &isDir);
    else f = SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);

    // delete the file/directory
    if (f != NULL)
    {
      // assemble the full names; trimming to MAX_PATH (2 * MAX_PATH) is theoretically unnecessary
      // but unfortunately required in practice
      lstrcpyn(end, f->Name, endSize);
      lstrcpyn(endDFSName, f->Name, endDFSNameSize);

      if (isDir)
      {
        BOOL skip = FALSE;
        if (ConfirmOnSystemHiddenDirDelete &&
            (f->Attr & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN)))
        {
          if (!skipAllSHDD && !yesAllSHDD)
          {
            int res = SalamanderGeneral->DialogQuestion(parent, BUTTONS_YESALLSKIPCANCEL, dfsFileName,
                                                        "Do you want to delete the directory with "
                                                        "SYSTEM or HIDDEN attribute?",
                                                        "Confirm Directory Delete");
            switch (res)
            {
              case DIALOG_ALL: yesAllSHDD = TRUE;
              case DIALOG_YES: break;

              case DIALOG_SKIPALL: skipAllSHDD = TRUE;
              case DIALOG_SKIP: skip = TRUE; break;

              default: success = FALSE; break; // DIALOG_CANCEL
            }
          }
          else  // skip all or delete all
          {
            if (skipAllSHDD) skip = TRUE;
          }
        }

        if (success && !skip)   // not canceled and not skipped
        {

          // handle ConfirmOnNotEmptyDirDelete plus recursive delete here,
          // also update the progress (after deleting/skipping files/directories)
          // deleted files should call SalamanderGeneral->RemoveOneFileFromCache();

          changeInSubdirs = TRUE;   // changes may also occur in subdirectories
        }
      }
      else
      {
        BOOL skip = FALSE;
        if (ConfirmOnSystemHiddenFileDelete &&
            (f->Attr & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN)))
        {
          if (!skipAllSHFD && !yesAllSHFD)
          {
            int res = SalamanderGeneral->DialogQuestion(parent, BUTTONS_YESALLSKIPCANCEL, dfsFileName,
                                                        "Do you want to delete the file with "
                                                        "SYSTEM or HIDDEN attribute?",
                                                        "Confirm File Delete");
            switch (res)
            {
              case DIALOG_ALL: yesAllSHFD = TRUE;
              case DIALOG_YES: break;

              case DIALOG_SKIPALL: skipAllSHFD = TRUE;
              case DIALOG_SKIP: skip = TRUE; break;

              default: success = FALSE; break; // DIALOG_CANCEL
            }
          }
          else  // skip all or delete all
          {
            if (skipAllSHFD) skip = TRUE;
          }
        }

        if (success && !skip)   // not canceled and not skipped
        {
          BOOL skip = FALSE;
          while (1)
          {
            SalamanderGeneral->ClearReadOnlyAttr(fileName, f->Attr);  // allow deletion of read-only items
            if (!DeleteFile(fileName))
            {
              if (!skipAllErrors)
              {
                SalamanderGeneral->GetErrorText(GetLastError(), buf, 2 * MAX_PATH);
                int res = SalamanderGeneral->DialogError(parent, BUTTONS_RETRYSKIPCANCEL, dfsFileName, buf, "DFS Delete Error");
                switch (res)
                {
                  case DIALOG_RETRY: break;

                  case DIALOG_SKIPALL: skipAllErrors = TRUE;
                  case DIALOG_SKIP: skip = TRUE; break;

                  default: success = FALSE; break; // DIALOG_CANCEL
                }
              }
              else skip = TRUE;
            }
            else
            {
              // filenames on disk are case-insensitive, the disk cache is case-sensitive, converting
              // to lowercase makes the disk cache behave case-insensitively as well
              SalamanderGeneral->ToLowerCase(dfsFileName);
              // remove the deleted file's copy from the disk cache (if it is cached)
              SalamanderGeneral->RemoveOneFileFromCache(dfsFileName);
              break;   // delete succeeded
            }
            if (!success || skip) break;
          }

          if (success)
          {

            // update the progress here (after deleting/skipping a single file)

          }
        }
      }
    }

    // check whether it makes sense to continue (if there is no error and another selected item exists)
    if (!success || focused || f == NULL) break;
  }

  // change on the Path path (without subdirectories if only files were deleted)
  // NOTE: a typical plugin should send the full FS path here
  SalamanderGeneral->PostChangeOnPathNotification(Path, changeInSubdirs);
  return success;
*/
}

BOOL WINAPI DFS_IsTheSamePath(const char* path1, const char* path2)
{
    while (*path1 != 0 && LowerCase[*path1] == LowerCase[*path2])
    {
        path1++;
        path2++;
    }
    if (*path1 == '\\')
        path1++;
    if (*path2 == '\\')
        path2++;
    return *path1 == 0 && *path2 == 0;
}

enum CDFSPathError
{
    dfspeNone,
    dfspeServerNameMissing,
    dfspeShareNameMissing,
    dfspeRelativePath, // relative paths are not supported ("PATH", "\PATH", or "C:PATH")
};

BOOL DFS_IsValidPath(const char* path, CDFSPathError* err)
{
    const char* s = path;
    if (err != NULL)
        *err = dfspeNone;
    if (*s == '\\' && *(s + 1) == '\\') // UNC (\\server\share\...)
    {
        s += 2;
        if (*s == 0 || *s == '\\')
        {
            if (err != NULL)
                *err = dfspeServerNameMissing;
        }
        else
        {
            while (*s != 0 && *s != '\\')
                s++; // skip the server name
            if (*s == '\\')
                s++;
            if (*s == 0 || *s == '\\')
            {
                if (err != NULL)
                    *err = dfspeShareNameMissing;
            }
            else
                return TRUE; // path OK
        }
    }
    else // path specified via a drive (c:\...)
    {
        if (LowerCase[*s] >= 'a' && LowerCase[*s] <= 'z' && *(s + 1) == ':' && *(s + 2) == '\\') // "c:\..."
        {
            return TRUE; // path OK
        }
        else
        {
            if (err != NULL)
                *err = dfspeRelativePath;
        }
    }
    return FALSE;
}

BOOL WINAPI
CPluginFSInterface::CopyOrMoveFromFS(BOOL copy, int mode, const char* fsName, HWND parent,
                                     int panel, int selectedFiles, int selectedDirs,
                                     char* targetPath, BOOL& operationMask,
                                     BOOL& cancelOrHandlePath, HWND dropTarget)
{
    operationMask = FALSE;
    cancelOrHandlePath = FALSE;

    if (mode == 1) // první volání: nech Salamander nabídnout cíl a zobrazit standardní dialog
        return FALSE;
    if (mode == 4) // chyba zpracování cesty -> nech uživatele opravit
        return FALSE;

    // ořízni masku (*.* apod.) z cílové cesty, ponech adresář
    char target[2 * MAX_PATH];
    lstrcpyn(target, targetPath, 2 * MAX_PATH);
    {
        char* lastBs = strrchr(target, '\\');
        char* comp = (lastBs != NULL) ? lastBs + 1 : target;
        if (strchr(comp, '*') != NULL || strchr(comp, '?') != NULL)
        {
            if (lastBs != NULL)
                *lastBs = 0;
            else
                target[0] = 0;
        }
    }

    // cíl musí být cesta na disku (X:\... nebo \\server\...)
    BOOL diskPath = (target[0] != 0 && target[1] == ':') ||
                    (target[0] == '\\' && target[1] == '\\');

    if (!SftpEnsureConnected(parent))
    {
        cancelOrHandlePath = TRUE;
        return TRUE;
    }

    if (!diskPath)
    {
        // cíl je na SFTP (sftp:/...) -> kopie v rámci serveru přes dočasný soubor
        char* up = strchr(target, ':');
        char remoteTargetDir[MAX_PATH];
        lstrcpyn(remoteTargetDir, (up != NULL) ? up + 1 : target, MAX_PATH);
        for (char* p = remoteTargetDir; *p; p++)
            if (*p == '\\')
                *p = '/';
        SftpNormalize(remoteTargetDir);

        char tmpDir[MAX_PATH];
        GetTempPath(MAX_PATH, tmpDir);

        BOOL focusedF = (selectedFiles == 0 && selectedDirs == 0);
        int indexF = 0;
        BOOL isDirF = FALSE;
        BOOL okF = TRUE;
        const CFileData* ff;
        SftpProgressBegin(parent);
        while (1)
        {
            ff = focusedF ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDirF)
                          : SalamanderGeneral->GetPanelSelectedItem(panel, &indexF, &isDirF);
            if (ff == NULL)
                break;
            char src[MAX_PATH], dst[MAX_PATH], tmpItem[2 * MAX_PATH];
            SftpJoin(Path, ff->Name, src, MAX_PATH);
            SftpJoin(remoteTargetDir, ff->Name, dst, MAX_PATH);
            lstrcpyn(tmpItem, tmpDir, 2 * MAX_PATH);
            SalamanderGeneral->SalPathAppend(tmpItem, ff->Name, 2 * MAX_PATH);
            BOOL step = SftpDownloadRecursive(src, tmpItem, isDirF != 0) &&
                        SftpUploadRecursive(tmpItem, dst, isDirF != 0);
            LocalDeleteRecursive(tmpItem, isDirF != 0); // úklid temp
            if (!step)
            {
                if (g_OvrCancel)
                {
                    okF = FALSE;
                    break;
                }
                char eb[700];
                _snprintf_s(eb, _TRUNCATE, "Chyba při kopírování \"%s\":\n%s\n\nPokračovat?", ff->Name, SftpConn.LastError());
                if (SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_YESNO | MB_ICONEXCLAMATION) == IDNO)
                {
                    okF = FALSE;
                    break;
                }
            }
            else if (!copy)
                SftpDeleteRecursive(src, isDirF != 0); // Move -> smaž zdroj
            if (focusedF)
                break;
        }
        SftpProgressEnd();
        char fsfull2[MAX_PATH + 32];
        _snprintf_s(fsfull2, _TRUNCATE, "%s:%s", fsName, remoteTargetDir);
        SalamanderGeneral->PostChangeOnPathNotification(fsfull2, FALSE);
        if (!copy)
            SalamanderGeneral->PostChangeOnPathNotification(Path, TRUE);
        if (okF)
            targetPath[0] = 0;
        else
            cancelOrHandlePath = TRUE;
        return TRUE;
    }

    BOOL focused = (selectedFiles == 0 && selectedDirs == 0);
    int index = 0;
    BOOL isDir = FALSE;
    BOOL success = TRUE;
    const CFileData* f;
    SftpProgressBegin(parent);
    while (1)
    {
        f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                    : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
        if (f == NULL)
            break;
        char remote[MAX_PATH], local[2 * MAX_PATH];
        SftpJoin(Path, f->Name, remote, MAX_PATH);
        lstrcpyn(local, target, 2 * MAX_PATH);
        SalamanderGeneral->SalPathAppend(local, f->Name, 2 * MAX_PATH);
        if (!SftpDownloadRecursive(remote, local, isDir != 0))
        {
            if (g_OvrCancel)
            {
                success = FALSE;
                break;
            }
            char eb[700];
            _snprintf_s(eb, _TRUNCATE, "Chyba při stahování \"%s\":\n%s\n\nPokračovat dalšími položkami?",
                        f->Name, SftpConn.LastError());
            if (SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME),
                                                 MB_YESNO | MB_ICONEXCLAMATION) == IDNO)
            {
                success = FALSE;
                break;
            }
        }
        if (focused)
            break;
    }
    SftpProgressEnd();

    // přesun (Move): po úspěšném zkopírování smaž zdroj na SFTP
    if (success && !copy)
    {
        focused = (selectedFiles == 0 && selectedDirs == 0);
        index = 0;
        while (1)
        {
            f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                        : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
            if (f == NULL)
                break;
            char remote[MAX_PATH];
            SftpJoin(Path, f->Name, remote, MAX_PATH);
            SftpDeleteRecursive(remote, isDir != 0);
            if (focused)
                break;
        }
        SalamanderGeneral->PostChangeOnPathNotification(Path, TRUE);
    }

    if (success)
        targetPath[0] = 0;
    else
        cancelOrHandlePath = TRUE;
    return TRUE;
}

BOOL WINAPI
CPluginFSInterface::CopyOrMoveFromDiskToFS(BOOL copy, int mode, const char* fsName, HWND parent,
                                           const char* sourcePath, SalEnumSelection2 next,
                                           void* nextParam, int sourceFiles, int sourceDirs,
                                           char* targetPath, BOOL* invalidPathOrCancel)
{
    if (invalidPathOrCancel != NULL)
        *invalidPathOrCancel = FALSE;

    if (mode == 1)
    {
        // přidej masku *.* k cílové cestě (Salamander zobrazí standardní dialog)
        SalamanderGeneral->SalPathAppend(targetPath, "*.*", 2 * MAX_PATH);
        return TRUE;
    }

    // získej user-part cílové cesty (za "fsName:") a ořízni masku -> vzdálený adresář
    char remoteDir[MAX_PATH];
    char* up = strchr(targetPath, ':');
    lstrcpyn(remoteDir, (up != NULL) ? up + 1 : targetPath, MAX_PATH);
    for (char* p = remoteDir; *p; p++)
        if (*p == '\\')
            *p = '/';
    {
        char* lastSlash = strrchr(remoteDir, '/');
        char* comp = (lastSlash != NULL) ? lastSlash + 1 : remoteDir;
        if (strchr(comp, '*') != NULL || strchr(comp, '?') != NULL)
        {
            if (lastSlash != NULL)
                *lastSlash = 0;
            else
                remoteDir[0] = 0;
        }
    }
    SftpNormalize(remoteDir);

    if (!SftpEnsureConnected(parent))
    {
        if (invalidPathOrCancel != NULL)
            *invalidPathOrCancel = TRUE;
        return FALSE;
    }

    const char* name;
    const char* dosName;
    BOOL isDir;
    CQuadWord size;
    DWORD attr;
    FILETIME lastWrite;
    BOOL success = TRUE;
    SftpProgressBegin(parent);
    while ((name = next(NULL, 0, &dosName, &isDir, &size, &attr, &lastWrite, nextParam, NULL)) != NULL)
    {
        // 'name' je relativní jméno; plná lokální cesta = sourcePath + name
        char local[2 * MAX_PATH];
        lstrcpyn(local, sourcePath, 2 * MAX_PATH);
        SalamanderGeneral->SalPathAppend(local, name, 2 * MAX_PATH);
        // jméno bez cesty pro vzdálený cíl
        const char* base = strrchr(name, '\\');
        base = (base != NULL) ? base + 1 : name;
        char remote[MAX_PATH];
        SftpJoin(remoteDir, base, remote, MAX_PATH);
        if (!SftpUploadRecursive(local, remote, isDir != 0))
        {
            if (g_OvrCancel)
            {
                success = FALSE;
                break;
            }
            char eb[700];
            _snprintf_s(eb, _TRUNCATE, "Chyba při nahrávání \"%s\":\n%s\n\nPokračovat dalšími položkami?",
                        base, SftpConn.LastError());
            if (SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME),
                                                 MB_YESNO | MB_ICONEXCLAMATION) == IDNO)
            {
                success = FALSE;
                break;
            }
            continue;
        }
        // přesun (Move): po úspěšném nahrání smaž lokální zdroj
        if (!copy)
            LocalDeleteRecursive(local, isDir != 0);
    }
    SftpProgressEnd();

    // obnov panel s naší FS cestou (pokud je zobrazena)
    char fsfull[MAX_PATH + 32];
    _snprintf_s(fsfull, _TRUNCATE, "%s:%s", fsName, remoteDir);
    SalamanderGeneral->PostChangeOnPathNotification(fsfull, FALSE);

    if (!success && invalidPathOrCancel != NULL)
        *invalidPathOrCancel = TRUE;
    return success;
}

// dialog pro zadání oktalových práv (chmod)
static int g_ChmodOctal = 0644;
static INT_PTR CALLBACK ChmodDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND par = GetParent(hwnd);
        if (par != NULL)
            SalamanderGeneral->MultiMonCenterWindow(hwnd, par, TRUE);
        char b[16];
        _snprintf_s(b, _TRUNCATE, "%o", g_ChmodOctal & 0777);
        SetDlgItemText(hwnd, IDC_CHMODVAL, b);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            char b[16];
            GetDlgItemText(hwnd, IDC_CHMODVAL, b, sizeof(b));
            g_ChmodOctal = (int)strtol(b, NULL, 8);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL WINAPI
CPluginFSInterface::ChangeAttributes(const char* fsName, HWND parent, int panel,
                                     int selectedFiles, int selectedDirs)
{
    if (!SftpEnsureConnected(parent))
        return FALSE;

    // předvyplň právy prvního vybraného/zaostřeného prvku
    BOOL focused = (selectedFiles == 0 && selectedDirs == 0);
    int index = 0;
    BOOL isDir = FALSE;
    const CFileData* f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                                 : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
    if (f != NULL)
    {
        char remote[MAX_PATH];
        SftpJoin(Path, f->Name, remote, MAX_PATH);
        unsigned long m;
        if (SftpConn.GetPermissions(remote, m))
            g_ChmodOctal = (int)m;
    }

    if (DialogBoxParam(HLanguage, MAKEINTRESOURCE(IDD_CHMOD), parent, ChmodDlgProc, 0) != IDOK)
        return FALSE;
    unsigned long mode = (unsigned long)(g_ChmodOctal & 0777);

    // aplikuj na všechny vybrané (nebo zaostřený)
    focused = (selectedFiles == 0 && selectedDirs == 0);
    index = 0;
    BOOL success = TRUE;
    while (1)
    {
        f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                    : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
        if (f == NULL)
            break;
        char remote[MAX_PATH];
        SftpJoin(Path, f->Name, remote, MAX_PATH);
        if (!SftpConn.Chmod(remote, mode))
        {
            char eb[700];
            _snprintf_s(eb, _TRUNCATE, "Nelze změnit práva \"%s\":\n%s\n\nPokračovat?",
                        f->Name, SftpConn.LastError());
            if (SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME),
                                                 MB_YESNO | MB_ICONEXCLAMATION) == IDNO)
            {
                success = FALSE;
                break;
            }
        }
        if (focused)
            break;
    }
    SalamanderGeneral->PostChangeOnPathNotification(Path, FALSE);
    return success;
}

void WINAPI
CPluginFSInterface::ShowProperties(const char* fsName, HWND parent, int panel,
                                   int selectedFiles, int selectedDirs)
{
    if (!SftpEnsureConnected(parent))
        return;
    BOOL focused = (selectedFiles == 0 && selectedDirs == 0);
    int index = 0;
    BOOL isDir = FALSE;
    const CFileData* f = focused ? SalamanderGeneral->GetPanelFocusedItem(panel, &isDir)
                                 : SalamanderGeneral->GetPanelSelectedItem(panel, &index, &isDir);
    if (f == NULL)
        return;
    char remote[MAX_PATH];
    SftpJoin(Path, f->Name, remote, MAX_PATH);
    unsigned __int64 size;
    unsigned long perms, uid, gid, mtime;
    if (!SftpConn.StatFull(remote, size, perms, uid, gid, mtime))
    {
        char eb[600];
        _snprintf_s(eb, _TRUNCATE, "Nelze načíst vlastnosti:\n%s", SftpConn.LastError());
        SalamanderGeneral->SalMessageBox(parent, eb, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    char timeStr[64] = "?";
    if (mtime != 0)
    {
        ULONGLONG ll = (ULONGLONG)mtime * 10000000ULL + 116444736000000000ULL;
        FILETIME ft;
        ft.dwLowDateTime = (DWORD)ll;
        ft.dwHighDateTime = (DWORD)(ll >> 32);
        SYSTEMTIME st, lt;
        FileTimeToSystemTime(&ft, &st);
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
        _snprintf_s(timeStr, _TRUNCATE, "%02d.%02d.%04d %02d:%02d:%02d",
                    lt.wDay, lt.wMonth, lt.wYear, lt.wHour, lt.wMinute, lt.wSecond);
    }
    char rwx[12];
    const char* perm = "rwxrwxrwx";
    rwx[0] = isDir ? 'd' : '-';
    for (int i = 0; i < 9; i++)
        rwx[i + 1] = (perms & (1 << (8 - i))) ? perm[i] : '-';
    rwx[10] = 0;

    char info[1100];
    if (focused || (selectedFiles + selectedDirs) <= 1)
    {
        _snprintf_s(info, _TRUNCATE,
                    "Jméno:\t%s\nTyp:\t%s\nVelikost:\t%I64u B\nZměněno:\t%s\nPráva:\t%s  (%03o)\nVlastník (UID):\t%lu\nSkupina (GID):\t%lu\nCesta:\t%s",
                    f->Name, isDir ? "adresář" : "soubor", size, timeStr, rwx,
                    (unsigned)(perms & 0777), uid, gid, remote);
    }
    else
    {
        _snprintf_s(info, _TRUNCATE, "Vybráno: %d souborů, %d adresářů", selectedFiles, selectedDirs);
    }
    SalamanderGeneral->SalMessageBox(parent, info, "Vlastnosti", MB_OK | MB_ICONINFORMATION);
}

void WINAPI
CPluginFSInterface::ContextMenu(const char* fsName, HWND parent, int menuX, int menuY, int type,
                                int panel, int selectedFiles, int selectedDirs)
{
#ifndef SFTP_QUIET
    char bufText[100];
    sprintf(bufText, "Show context menu (type %d).", (int)type);
    SalamanderGeneral->SalMessageBox(parent, bufText, "DFS Context Menu", MB_OK | MB_ICONINFORMATION);
#endif // SFTP_QUIET

    HMENU menu = CreatePopupMenu();
    if (menu == NULL)
    {
        TRACE_E("CPluginFSInterface::ContextMenu: Unable to create menu.");
        return;
    }
    MENUITEMINFO mi;
    char nameBuf[200];

    switch (type)
    {
    case fscmItemsInPanel: // context menu for panel items (selected/focused files and directories)
    {
        int i = 0;

        int index = 0;
        int salCmd;
        BOOL enabled;
        int type2, lastType = sctyUnknown;
        while (SalamanderGeneral->EnumSalamanderCommands(&index, &salCmd, nameBuf, 200, &enabled, &type2))
        {
            if (type2 != lastType /*&& lastType != sctyUnknown*/) // insert a separator
            {
                memset(&mi, 0, sizeof(mi));
                mi.cbSize = sizeof(mi);
                mi.fMask = MIIM_TYPE;
                mi.fType = MFT_SEPARATOR;
                InsertMenuItem(menu, i++, TRUE, &mi);
            }
            lastType = type2;

            // insert Salamander commands
            memset(&mi, 0, sizeof(mi));
            mi.cbSize = sizeof(mi);
            mi.fMask = MIIM_TYPE | MIIM_ID | MIIM_STATE;
            mi.fType = MFT_STRING;
            mi.wID = salCmd + 1000; // shift Salamander commands by 1000 so they differ from ours
            mi.dwTypeData = nameBuf;
            mi.cch = (UINT)strlen(nameBuf);
            mi.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
            InsertMenuItem(menu, i++, TRUE, &mi);
        }
        DWORD cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                     menuX, menuY, parent, NULL);
        if (cmd >= 1000) // the user selected a Salamander command
        {
            if (SalamanderGeneral->GetSalamanderCommand(cmd - 1000, nameBuf, 200, &enabled, &type2))
                TRACE_I("Starting command: " << nameBuf);
            SalamanderGeneral->PostSalamanderCommand(cmd - 1000);
        }
        break;
    }

    case fscmPathInPanel: // context menu for the current path in the panel
    {
        int i = 0;

        strcpy(nameBuf, "&Odpojit");
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_TYPE | MIIM_ID | MIIM_STATE;
        mi.fType = MFT_STRING;
        mi.wID = panel == PANEL_LEFT ? MENUCMD_DISCONNECT_LEFT : MENUCMD_DISCONNECT_RIGHT;
        mi.dwTypeData = nameBuf;
        mi.cch = (UINT)strlen(nameBuf);
        mi.fState = MFS_ENABLED;
        InsertMenuItem(menu, i++, TRUE, &mi);

        DWORD cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                     menuX, menuY, parent, NULL);
        if (cmd != 0)                                         // the user selected a command from the menu
            SalamanderGeneral->PostMenuExtCommand(cmd, TRUE); // execute later in "sal-idle"
        break;
    }

    case fscmPanel: // context menu for the panel
    {
        int i = 0;

        strcpy(nameBuf, "&Odpojit");
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_TYPE | MIIM_ID | MIIM_STATE;
        mi.fType = MFT_STRING;
        mi.wID = panel == PANEL_LEFT ? MENUCMD_DISCONNECT_LEFT : MENUCMD_DISCONNECT_RIGHT;
        mi.dwTypeData = nameBuf;
        mi.cch = (UINT)strlen(nameBuf);
        mi.fState = MFS_ENABLED;
        InsertMenuItem(menu, i++, TRUE, &mi);

        DWORD cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                     menuX, menuY, parent, NULL);
        if (cmd != 0)                                         // the user selected a command from the menu
            SalamanderGeneral->PostMenuExtCommand(cmd, TRUE); // execute later in "sal-idle"
        break;
    }
    }
    DestroyMenu(menu);
}
