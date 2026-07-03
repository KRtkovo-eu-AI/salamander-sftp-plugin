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

// FS name assigned by Salamander after the plugin loads
char AssignedFSName[MAX_PATH] = "";
extern int AssignedFSNameLen = 0;

// image list for simple FS icons
HIMAGELIST DFSImageList = NULL;

// global variables used to cache pointers to Salamander-wide variables
// shared between the archiver and the FS
const CFileData** TransferFileData = NULL;
int* TransferIsDir = NULL;
char* TransferBuffer = NULL;
int* TransferLen = NULL;
DWORD* TransferRowData = NULL;
CPluginDataInterfaceAbstract** TransferPluginDataIface = NULL;
DWORD* TransferActCustomData = NULL;

// helper variable for tests
CPluginFSInterfaceAbstract* LastDetachedFS = NULL;

// structure used to hand over data from the "Connect" dialog to a newly created FS
CConnectData ConnectData;

// ****************************************************************************
// FILE SYSTEM SECTION
// ****************************************************************************

BOOL InitFS()
{
    DFSImageList = ImageList_Create(16, 16, ILC_MASK | SalamanderGeneral->GetImageListColorFlags(), 2, 0);
    if (DFSImageList == NULL)
    {
        TRACE_E("Unable to create image list.");
        return FALSE;
    }
    ImageList_SetImageCount(DFSImageList, 2); // initialize
    ImageList_SetBkColor(DFSImageList, SalamanderGeneral->GetCurrentColor(SALCOL_ITEM_BK_NORMAL));

    // icons differ across Windows versions, ideally we would load them dynamically (e.g. the
    // directory icon from the system directory); here we simply use a single icon variant
    // (which does not match Windows 2000 for example)
    ImageList_ReplaceIcon(DFSImageList, 0, HANDLES(LoadIcon(DLLInstance, MAKEINTRESOURCE(IDI_DIR))));
    ImageList_ReplaceIcon(DFSImageList, 1, HANDLES(LoadIcon(DLLInstance, MAKEINTRESOURCE(IDI_FILE))));

    return TRUE;
}

void ReleaseFS()
{
    ImageList_Destroy(DFSImageList);
}

//
// ****************************************************************************
// CPluginInterfaceForFS
//

CPluginFSInterfaceAbstract* WINAPI
CPluginInterfaceForFS::OpenFS(const char* fsName, int fsNameIndex)
{

    // this is where a dedicated FS object should be created for each fsNameIndex...

    // 'fsName' shows how the user typed the FS name ("Ftp", "ftp", "FTP",
    // etc. - it is still the same FS name)

    ActiveFSCount++;
    return new CPluginFSInterface;
}

void WINAPI
CPluginInterfaceForFS::CloseFS(CPluginFSInterfaceAbstract* fs)
{
    CPluginFSInterface* dfsFS = (CPluginFSInterface*)fs; // to ensure the correct destructor is invoked

    if (dfsFS == LastDetachedFS)
        LastDetachedFS = NULL;
    ActiveFSCount--;
    if (dfsFS != NULL)
        delete dfsFS;
}

char ConnectPath[MAX_PATH] = "";
char** History = NULL;
int HistoryCount = 0;

// stav zobrazení hesla (tlačítko oko)
static bool g_PwdShown = false;
static HFONT g_EyeFont = NULL;

// naplň seznam uložených spojení
// je daná složka už v seznamu složek?
static bool FolderKnown(const char* name)
{
    for (int i = 0; i < SftpFolderCount; i++)
        if (strcmp(SftpFolders[i], name) == 0)
            return true;
    return false;
}

static void FillSessionList(HWND HWindow)
{
    HWND lb = GetDlgItem(HWindow, IDC_SESSIONS);
    SendMessage(lb, LB_RESETCONTENT, 0, 0);

    // doplň do seznamu složek i ty, které jsou jen u profilů
    for (int i = 0; i < SftpProfileCount; i++)
        if (SftpProfiles[i].Folder[0] != 0 && !FolderKnown(SftpProfiles[i].Folder) && SftpFolderCount < SFTP_MAX_FOLDERS)
            lstrcpyn(SftpFolders[SftpFolderCount++], SftpProfiles[i].Folder, 128);

    // 1) spojení v kořeni
    for (int i = 0; i < SftpProfileCount; i++)
        if (SftpProfiles[i].Folder[0] == 0)
        {
            const char* nm = SftpProfiles[i].Name[0] != 0 ? SftpProfiles[i].Name : SftpProfiles[i].Host;
            int li = (int)SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)nm);
            SendMessage(lb, LB_SETITEMDATA, li, i);
        }
    // 2) složky a jejich obsah
    for (int fi = 0; fi < SftpFolderCount; fi++)
    {
        char hdr[160];
        _snprintf_s(hdr, _TRUNCATE, "[ %s ]", SftpFolders[fi]);
        int hl = (int)SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)hdr);
        SendMessage(lb, LB_SETITEMDATA, hl, (LPARAM)-1); // hlavička složky (není profil)
        for (int i = 0; i < SftpProfileCount; i++)
            if (strcmp(SftpProfiles[i].Folder, SftpFolders[fi]) == 0)
            {
                char line[220];
                const char* nm = SftpProfiles[i].Name[0] != 0 ? SftpProfiles[i].Name : SftpProfiles[i].Host;
                _snprintf_s(line, _TRUNCATE, "      %s", nm);
                int li = (int)SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)line);
                SendMessage(lb, LB_SETITEMDATA, li, i);
            }
    }
}

// vrátí index profilu vybraného v seznamu (mapování přes item-data), nebo -1
static int GetSelProfile(HWND HWindow)
{
    int li = (int)SendDlgItemMessage(HWindow, IDC_SESSIONS, LB_GETCURSEL, 0, 0);
    if (li < 0)
        return -1;
    int idx = (int)SendDlgItemMessage(HWindow, IDC_SESSIONS, LB_GETITEMDATA, li, 0);
    return (idx >= 0 && idx < SftpProfileCount) ? idx : -1;
}

// zjistí složku podle aktuálního výběru (hlavička složky / spojení ve složce), jinak ""
static void GetSelFolder(HWND HWindow, char* out, int outSize)
{
    out[0] = 0;
    HWND lb = GetDlgItem(HWindow, IDC_SESSIONS);
    int li = (int)SendMessage(lb, LB_GETCURSEL, 0, 0);
    if (li < 0)
        return;
    int idx = (int)SendMessage(lb, LB_GETITEMDATA, li, 0);
    if (idx >= 0 && idx < SftpProfileCount)
    {
        lstrcpyn(out, SftpProfiles[idx].Folder, outSize);
        return;
    }
    char txt[220];
    SendMessage(lb, LB_GETTEXT, li, (LPARAM)txt);
    char* s = strstr(txt, "[ ");
    if (s != NULL)
    {
        s += 2;
        char* e = strstr(s, " ]");
        if (e != NULL)
            *e = 0;
        lstrcpyn(out, s, outSize);
    }
}

// načti profil do polí dialogu
static void LoadProfileToFields(HWND HWindow, const CSftpSavedProfile& p)
{
    char portStr[16];
    SetDlgItemText(HWindow, IDC_HOST, p.Host);
    _snprintf_s(portStr, _TRUNCATE, "%d", p.Port > 0 ? p.Port : 22);
    SetDlgItemText(HWindow, IDC_PORT, portStr);
    SetDlgItemText(HWindow, IDC_USER, p.User);
    SetDlgItemText(HWindow, IDC_PASSWORD, p.Password);
    SetDlgItemText(HWindow, IDC_KEYFILE, p.KeyFile);
    SetDlgItemText(HWindow, IDC_PATH, p.Path[0] != 0 ? p.Path : "/");
    CheckDlgButton(HWindow, IDC_SSHCOMPRESS, p.UseCompression ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(HWindow, IDC_SCPFALLBACK, p.ScpFallback ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessage(HWindow, IDC_PROTOCOL, CB_SETCURSEL, p.Protocol == 1 ? 1 : 0, 0);
}

// --- jednoduchý vstupní dialog (keyboard-interactive) ---
struct CInputData
{
    const char* Prompt;
    bool Echo;
    char* Out;
    int OutSize;
};

static INT_PTR CALLBACK InputDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        CInputData* d = (CInputData*)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemText(hWnd, IDC_INPUTPROMPT, d->Prompt);
        if (!d->Echo)
            SendDlgItemMessage(hWnd, IDC_INPUTVAL, EM_SETPASSWORDCHAR, (WPARAM)'*', 0);
        HWND p = GetParent(hWnd);
        if (p != NULL)
            SalamanderGeneral->MultiMonCenterWindow(hWnd, p, TRUE);
        SetFocus(GetDlgItem(hWnd, IDC_INPUTVAL));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            CInputData* d = (CInputData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            GetDlgItemText(hWnd, IDC_INPUTVAL, d->Out, d->OutSize);
            EndDialog(hWnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hWnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool SftpInputDialog(HWND parent, const char* prompt, bool echo, char* out, int outSize)
{
    CInputData d;
    d.Prompt = prompt;
    d.Echo = echo;
    d.Out = out;
    d.OutSize = outSize;
    if (outSize > 0) out[0] = 0;
    return DialogBoxParam(HLanguage, MAKEINTRESOURCE(IDD_INPUT), parent, InputDlgProc, (LPARAM)&d) == IDOK;
}

// na kterou stránku patří daný ovládací prvek (-1 = vždy viditelný)
static int ControlPage(int id)
{
    switch (id)
    {
    case IDC_ST_PAGE0_CONNECTION:
    case IDC_ST_PAGE0_HOST:
    case IDC_ST_PAGE0_PORT:
    case IDC_ST_PAGE0_USER:
    case IDC_ST_PAGE0_PASSWORD:
    case IDC_ST_PAGE0_KEYFILE:
    case IDC_ST_PAGE0_PROTOCOL:
    case IDC_ST_PAGE0_PROTOCOL_LABEL:
    case IDC_HOST:
    case IDC_PORT:
    case IDC_USER:
    case IDC_PASSWORD:
    case IDC_KEYFILE:
    case IDC_KEYBROWSE:
    case IDC_PROTOCOL:
    case IDC_SCPFALLBACK:
    case IDC_PWDEYE:
        return 0; // Spojení
    case IDC_SESSIONS:
    case IDC_DELSESSION:
    case IDC_SESS_NEW:
    case IDC_SESS_EDIT:
    case IDC_SESS_RENAME:
    case IDC_SESS_FOLDER:
    case IDC_SESS_DEFAULT:
        return 1; // Uložená spojení
    case IDC_ST_PAGE2_DIR:
    case IDC_PATH:
        return 2; // Adresáře
    case IDC_ST_PAGE3_ENCRYPTION:
    case IDC_ST_PAGE3_ENCODING:
    case IDC_ST_PAGE3_CIPHERS:
    case IDC_SSHCOMPRESS:
    case IDC_ENCODING:
        return 3; // SSH (šifrování a komprese)
    default:
        return -1; // strom, Pokročilé volby, tlačítka -> vždy
    }
}

static int g_ActivePage = 0;
static BOOL CALLBACK PageToggleProc(HWND hChild, LPARAM page)
{
    int p = ControlPage(GetDlgCtrlID(hChild));
    if (p >= 0)
        ShowWindow(hChild, p == (int)page ? SW_SHOW : SW_HIDE);
    return TRUE;
}

// přepne dialog na stránku (0=Spojení, 1=Uložená spojení, 2=Adresáře, 3=SSH)
static void SwitchConnectPage(HWND HWindow, int page)
{
    g_ActivePage = page;
    EnumChildWindows(HWindow, PageToggleProc, page);
}

// naplní strom kategorií; 'advanced' přidá pokročilé stránky (Adresáře, SSH)
static void BuildConnectTree(HWND tree, bool advanced)
{
    TreeView_DeleteAllItems(tree);
    TVINSERTSTRUCT tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hParent = TVI_ROOT;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    HTREEITEM hConn;
    tvi.item.pszText = (LPSTR) "Spojení";
    tvi.item.lParam = 0;
    hConn = TreeView_InsertItem(tree, &tvi);
    tvi.item.pszText = (LPSTR) "Uložená spojení";
    tvi.item.lParam = 1;
    TreeView_InsertItem(tree, &tvi);
    if (advanced)
    {
        tvi.item.pszText = (LPSTR) "Adresáře";
        tvi.item.lParam = 2;
        TreeView_InsertItem(tree, &tvi);
        tvi.item.pszText = (LPSTR) "SSH";
        tvi.item.lParam = 3;
        TreeView_InsertItem(tree, &tvi);
    }
    TreeView_SelectItem(tree, hConn);
}

INT_PTR CALLBACK ConnectDlgProc(HWND HWindow, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CALL_STACK_MESSAGE4("ConnectDlgProc(, 0x%X, 0x%IX, 0x%IX)", uMsg, wParam, lParam);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        HWND hParent = GetParent(HWindow);
        if (hParent != NULL)
            SalamanderGeneral->MultiMonCenterWindow(HWindow, hParent, TRUE);

        FillSessionList(HWindow);

        // naplň combo protokolu (SFTP je výchozí, SCP nouzově)
        HWND combo = GetDlgItem(HWindow, IDC_PROTOCOL);
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM) "SFTP");
        SendMessage(combo, CB_ADDSTRING, 0, (LPARAM) "SCP");
        SendMessage(combo, CB_SETCURSEL, 0, 0);

        // combo kódování názvů
        HWND enc = GetDlgItem(HWindow, IDC_ENCODING);
        SendMessage(enc, CB_ADDSTRING, 0, (LPARAM) "Automaticky (UTF-8)");
        SendMessage(enc, CB_ADDSTRING, 0, (LPARAM) "UTF-8");
        SendMessage(enc, CB_ADDSTRING, 0, (LPARAM) "Vypnuto (bez převodu)");
        SendMessage(enc, CB_SETCURSEL, (SftpEncoding >= 0 && SftpEncoding <= 2) ? SftpEncoding : 0, 0);

        // naplň strom kategorií (jako v původním WinSCP); pokročilé stránky skryté
        BuildConnectTree(GetDlgItem(HWindow, IDC_CATTREE), false);

        // předvyplň z posledního profilu
        SetDlgItemText(HWindow, IDC_HOST, SftpProfile.Host);
        char portStr[16];
        _snprintf_s(portStr, _TRUNCATE, "%d", SftpProfile.Port > 0 ? SftpProfile.Port : 22);
        SetDlgItemText(HWindow, IDC_PORT, portStr);
        SetDlgItemText(HWindow, IDC_USER, SftpProfile.User);
        SetDlgItemText(HWindow, IDC_PASSWORD, SftpProfile.Password);
        SetDlgItemText(HWindow, IDC_KEYFILE, SftpProfile.KeyFile);
        SetDlgItemText(HWindow, IDC_PATH, ConnectPath[0] != 0 ? ConnectPath : "/");
        CheckDlgButton(HWindow, IDC_SSHCOMPRESS, SftpProfile.UseCompression ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(HWindow, IDC_SCPFALLBACK, SftpProfile.ScpFallback ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessage(HWindow, IDC_PROTOCOL, CB_SETCURSEL, SftpProfile.Protocol == 1 ? 1 : 0, 0);

        // pokud je nastaveno výchozí spojení, předvyplň ho
        if (SftpDefaultSession[0] != 0)
        {
            for (int i = 0; i < SftpProfileCount; i++)
                if (strcmp(SftpProfiles[i].Name, SftpDefaultSession) == 0)
                {
                    LoadProfileToFields(HWindow, SftpProfiles[i]);
                    SendDlgItemMessage(HWindow, IDC_SESSIONS, LB_SETCURSEL, i, 0);
                    break;
                }
        }

        // tlačítko "oko" u hesla (glyf z fontu Segoe MDL2 Assets)
        g_PwdShown = false;
        if (g_EyeFont == NULL)
            g_EyeFont = CreateFontA(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, "Segoe MDL2 Assets");
        {
            HWND eye = GetDlgItem(HWindow, IDC_PWDEYE);
            SendMessage(eye, WM_SETFONT, (WPARAM)g_EyeFont, TRUE);
            SetWindowTextW(eye, L"\xE7B3"); // glyf "oko" (RedEye)
        }

        SwitchConnectPage(HWindow, 0); // začni na formuláři Spojení
        return TRUE;
    }

    case WM_NOTIFY:
    {
        LPNMHDR nmh = (LPNMHDR)lParam;
        if (nmh->idFrom == IDC_CATTREE && nmh->code == TVN_SELCHANGED)
        {
            LPNMTREEVIEW nmtv = (LPNMTREEVIEW)lParam;
            SwitchConnectPage(HWindow, (int)nmtv->itemNew.lParam);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDC_PWDEYE:
        {
            // přepni zobrazení hesla (hvězdičky <-> čitelně)
            g_PwdShown = !g_PwdShown;
            HWND pwd = GetDlgItem(HWindow, IDC_PASSWORD);
            SendMessage(pwd, EM_SETPASSWORDCHAR, g_PwdShown ? 0 : (WPARAM)'*', 0);
            // edit se po změně password-char sám nepřekreslí → vynuť re-setem textu
            int len = GetWindowTextLength(pwd);
            char* tmp = new char[len + 1];
            GetWindowText(pwd, tmp, len + 1);
            SetWindowText(pwd, tmp);
            delete[] tmp;
            InvalidateRect(pwd, NULL, TRUE);
            SetWindowTextW(GetDlgItem(HWindow, IDC_PWDEYE), g_PwdShown ? L"\xE7B3" : L"\xE7B3"); // oko
            return TRUE;
        }

        case IDC_ADVANCED:
        {
            // přepni zobrazení pokročilých kategorií (Adresáře, SSH) ve stromu
            bool adv = IsDlgButtonChecked(HWindow, IDC_ADVANCED) == BST_CHECKED;
            BuildConnectTree(GetDlgItem(HWindow, IDC_CATTREE), adv);
            SwitchConnectPage(HWindow, 0); // po přestavbě ukaž Spojení
            return TRUE;
        }

        case IDC_SESSIONS:
        {
            int sel = GetSelProfile(HWindow);
            if (sel >= 0)
            {
                if (HIWORD(wParam) == LBN_SELCHANGE)
                    LoadProfileToFields(HWindow, SftpProfiles[sel]);
                else if (HIWORD(wParam) == LBN_DBLCLK)
                {
                    LoadProfileToFields(HWindow, SftpProfiles[sel]);
                    PostMessage(HWindow, WM_COMMAND, IDOK, 0); // dvojklik = připojit
                }
            }
            return TRUE;
        }

        case IDC_SESS_NEW:
        {
            // nové spojení: vyprázdni pole a přepni na stránku Spojení
            SetDlgItemText(HWindow, IDC_HOST, "");
            SetDlgItemText(HWindow, IDC_PORT, "22");
            SetDlgItemText(HWindow, IDC_USER, "");
            SetDlgItemText(HWindow, IDC_PASSWORD, "");
            SetDlgItemText(HWindow, IDC_KEYFILE, "");
            SetDlgItemText(HWindow, IDC_PATH, "/");
            CheckDlgButton(HWindow, IDC_SSHCOMPRESS, BST_UNCHECKED);
            CheckDlgButton(HWindow, IDC_SCPFALLBACK, BST_UNCHECKED);
            SendDlgItemMessage(HWindow, IDC_PROTOCOL, CB_SETCURSEL, 0, 0);
            HWND tree = GetDlgItem(HWindow, IDC_CATTREE);
            TreeView_SelectItem(tree, TreeView_GetRoot(tree)); // přepne na Spojení
            return TRUE;
        }

        case IDC_SESS_EDIT:
        {
            // upravit: načti vybraný profil do polí a přepni na Spojení
            int sel = GetSelProfile(HWindow);
            if (sel < 0)
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Nejdřív vyberte spojení v seznamu.",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            LoadProfileToFields(HWindow, SftpProfiles[sel]);
            HWND tree = GetDlgItem(HWindow, IDC_CATTREE);
            TreeView_SelectItem(tree, TreeView_GetRoot(tree));
            return TRUE;
        }

        case IDC_SESS_RENAME:
        {
            int sel = GetSelProfile(HWindow);
            if (sel < 0)
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Nejdřív vyberte spojení v seznamu.",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            char name[128];
            lstrcpyn(name, SftpProfiles[sel].Name, sizeof(name));
            if (SftpInputDialog(HWindow, "Nový název spojení:", true, name, sizeof(name)) && name[0] != 0)
            {
                lstrcpyn(SftpProfiles[sel].Name, name, sizeof(SftpProfiles[sel].Name));
                FillSessionList(HWindow);
                SendDlgItemMessage(HWindow, IDC_SESSIONS, LB_SETCURSEL, sel, 0);
            }
            return TRUE;
        }

        case IDC_SESS_FOLDER:
        {
            // vytvoř novou složku; pokud je vybráno spojení, přesuň ho do ní
            char name[128] = "";
            if (!SftpInputDialog(HWindow, "Název nové složky:", true, name, sizeof(name)) || name[0] == 0)
                return TRUE;
            if (!FolderKnown(name) && SftpFolderCount < SFTP_MAX_FOLDERS)
                lstrcpyn(SftpFolders[SftpFolderCount++], name, 128);
            int sel = GetSelProfile(HWindow);
            if (sel >= 0)
                lstrcpyn(SftpProfiles[sel].Folder, name, sizeof(SftpProfiles[sel].Folder)); // přesuň vybrané do složky
            FillSessionList(HWindow);
            return TRUE;
        }

        case IDC_SESS_DEFAULT:
        {
            int sel = GetSelProfile(HWindow);
            if (sel < 0)
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Nejdřív vyberte spojení v seznamu.",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            lstrcpyn(SftpDefaultSession, SftpProfiles[sel].Name, sizeof(SftpDefaultSession));
            char msg[256];
            _snprintf_s(msg, _TRUNCATE, "Spojení \"%s\" nastaveno jako výchozí.", SftpProfiles[sel].Name);
            SalamanderGeneral->SalMessageBox(HWindow, msg, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }

        case IDC_KEYBROWSE:
        {
            char file[MAX_PATH];
            GetDlgItemText(HWindow, IDC_KEYFILE, file, MAX_PATH);
            OPENFILENAME ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = HWindow;
            ofn.lpstrFilter = "Privátní klíče\0*.ppk;*.pem;id_*;*.key\0Všechny soubory\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle = "Vyberte soubor privátního klíče";
            if (GetOpenFileName(&ofn))
                SetDlgItemText(HWindow, IDC_KEYFILE, file);
            return TRUE;
        }

        case IDC_SAVESESSION:
        {
            // ulož aktuální pole jako profil (jméno = user@host:port)
            CSftpSavedProfile p;
            memset(&p, 0, sizeof(p));
            GetDlgItemText(HWindow, IDC_HOST, p.Host, sizeof(p.Host));
            char portStr[16];
            GetDlgItemText(HWindow, IDC_PORT, portStr, sizeof(portStr));
            p.Port = atoi(portStr);
            if (p.Port <= 0)
                p.Port = 22;
            GetDlgItemText(HWindow, IDC_USER, p.User, sizeof(p.User));
            GetDlgItemText(HWindow, IDC_PASSWORD, p.Password, sizeof(p.Password));
            GetDlgItemText(HWindow, IDC_KEYFILE, p.KeyFile, sizeof(p.KeyFile));
            GetDlgItemText(HWindow, IDC_PATH, p.Path, sizeof(p.Path));
            p.UseCompression = IsDlgButtonChecked(HWindow, IDC_SSHCOMPRESS) == BST_CHECKED;
            p.ScpFallback = IsDlgButtonChecked(HWindow, IDC_SCPFALLBACK) == BST_CHECKED;
            p.Protocol = (int)SendDlgItemMessage(HWindow, IDC_PROTOCOL, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
            GetSelFolder(HWindow, p.Folder, sizeof(p.Folder)); // ulož do aktuálně vybrané složky
            if (p.Host[0] == 0)
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Nejdřív zadejte hostitele.",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            if (p.User[0] != 0)
                _snprintf_s(p.Name, _TRUNCATE, "%s@%s:%d", p.User, p.Host, p.Port);
            else
                _snprintf_s(p.Name, _TRUNCATE, "%s:%d", p.Host, p.Port);
            // existuje už profil se stejným jménem? -> přepiš
            int found = -1;
            for (int i = 0; i < SftpProfileCount; i++)
                if (strcmp(SftpProfiles[i].Name, p.Name) == 0)
                {
                    found = i;
                    break;
                }
            if (found >= 0)
                SftpProfiles[found] = p;
            else if (SftpProfileCount < SFTP_MAX_PROFILES)
                SftpProfiles[SftpProfileCount++] = p;
            else
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Dosažen maximální počet uložených spojení.",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            FillSessionList(HWindow);
            return TRUE;
        }

        case IDC_DELSESSION:
        {
            int sel = GetSelProfile(HWindow);
            if (sel >= 0)
            {
                for (int i = sel; i < SftpProfileCount - 1; i++)
                    SftpProfiles[i] = SftpProfiles[i + 1];
                SftpProfileCount--;
                FillSessionList(HWindow);
            }
            return TRUE;
        }

        case IDOK:
        {
            GetDlgItemText(HWindow, IDC_HOST, SftpProfile.Host, sizeof(SftpProfile.Host));
            char portStr[16];
            GetDlgItemText(HWindow, IDC_PORT, portStr, sizeof(portStr));
            SftpProfile.Port = atoi(portStr);
            if (SftpProfile.Port <= 0)
                SftpProfile.Port = 22;
            GetDlgItemText(HWindow, IDC_USER, SftpProfile.User, sizeof(SftpProfile.User));
            GetDlgItemText(HWindow, IDC_PASSWORD, SftpProfile.Password, sizeof(SftpProfile.Password));
            GetDlgItemText(HWindow, IDC_KEYFILE, SftpProfile.KeyFile, sizeof(SftpProfile.KeyFile));
            SftpProfile.UseCompression = IsDlgButtonChecked(HWindow, IDC_SSHCOMPRESS) == BST_CHECKED;
            SftpProfile.ScpFallback = IsDlgButtonChecked(HWindow, IDC_SCPFALLBACK) == BST_CHECKED;
            SftpProfile.Protocol = (int)SendDlgItemMessage(HWindow, IDC_PROTOCOL, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
            {
                int e = (int)SendDlgItemMessage(HWindow, IDC_ENCODING, CB_GETCURSEL, 0, 0);
                if (e >= 0 && e <= 2)
                    SftpEncoding = e;
            }
            GetDlgItemText(HWindow, IDC_PATH, ConnectPath, MAX_PATH);
            if (ConnectPath[0] == 0)
                strcpy(ConnectPath, "/");
            if (SftpProfile.Host[0] == 0)
            {
                SalamanderGeneral->SalMessageBox(HWindow, "Zadejte adresu serveru (hostitele).",
                                                 LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            SftpProfile.Valid = true;
            SftpConn.Disconnect(); // nové připojení -> zavři staré
            EndDialog(HWindow, IDOK);
            return TRUE;
        }
        case IDCANCEL:
        {
            EndDialog(HWindow, IDCANCEL);
            return TRUE;
        }
        }
        break;
    }
    }
    return FALSE; // not processed
}

void WINAPI
CPluginInterfaceForFS::ExecuteChangeDriveMenuItem(int panel)
{
    CALL_STACK_MESSAGE2("CPluginInterfaceForFS::ExecuteChangeDriveMenuItem(%d)", panel);
    SalamanderGeneral->GetStdHistoryValues(SALHIST_CHANGEDIR, &History, &HistoryCount);
    while (1)
    {
        if (DialogBoxParam(HLanguage, MAKEINTRESOURCE(IDD_CONNECT),
                           SalamanderGeneral->GetMsgBoxParent(), ConnectDlgProc, NULL) == IDOK)
        {
            // repaint the main window so the user does not keep staring at stale content after the dialog
            UpdateWindow(SalamanderGeneral->GetMainWindowHWND());

            // change the active panel path to AssignedFSName:ConnectPath
            ConnectData.UseConnectData = TRUE;
            lstrcpyn(ConnectData.UserPart, ConnectPath, MAX_PATH);
            int failReason;
            BOOL changeRes = SalamanderGeneral->ChangePanelPathToPluginFS(panel, AssignedFSName, "", &failReason);
            // NOTE: on success it returns failReason==CHPPFR_SHORTERPATH (the user-part of the path is not empty)
            ConnectData.UseConnectData = FALSE;
            if (!changeRes && failReason == CHPPFR_INVALIDPATH)
                continue; // repeat the prompt

            /*
      if (!SalamanderGeneral->ChangePanelPathToDisk(panel, ConnectPath, &failReason))
      {
        if (failReason == CHPPFR_INVALIDPATH) continue;  // repeat the prompt
      }
*/
            /*
      if (!SalamanderGeneral->ChangePanelPathToArchive(panel, ConnectPath, "", &failReason))
      {
        if (failReason == CHPPFR_INVALIDPATH || failReason == CHPPFR_INVALIDARCHIVE) continue;  // repeat the prompt
      }
*/
            /*
      if (LastDetachedFS != NULL &&
          !SalamanderGeneral->ChangePanelPathToDetachedFS(panel, LastDetachedFS, &failReason))
      {
        // repeating the prompt makes no sense
      }
*/
            /*
      char buf[MAX_PATH];
      if (SalamanderGeneral->GetLastWindowsPanelPath(panel, buf, MAX_PATH))
      {
        if (!SalamanderGeneral->ChangePanelPathToDisk(panel, buf, &failReason))
        {
          // repeating the prompt makes no sense
        }
      }
*/
            /*
      if (!SalamanderGeneral->ChangePanelPathToRescuePathOrFixedDrive(panel, &failReason))
      {
        // repeating the prompt makes no sense
      }
*/
            //      SalamanderGeneral->RefreshPanelPath(panel);
            //      SalamanderGeneral->PostRefreshPanelPath(panel);
            /*
      if (LastDetachedFS != NULL)
      {
        SalamanderGeneral->CloseDetachedFS(SalamanderGeneral->GetMsgBoxParent(), LastDetachedFS);
      }
*/
        }
        break;
    }
}

BOOL WINAPI
CPluginInterfaceForFS::ChangeDriveMenuItemContextMenu(HWND parent, int panel, int x, int y,
                                                      CPluginFSInterfaceAbstract* pluginFS,
                                                      const char* pluginFSName, int pluginFSNameIndex,
                                                      BOOL isDetachedFS, BOOL& refreshMenu,
                                                      BOOL& closeMenu, int& postCmd, void*& postCmdParam)
{
    CALL_STACK_MESSAGE7("CPluginInterfaceForFS::ChangeDriveMenuItemContextMenu(, %d, %d, %d, , %s, %d, %d, , , ,)",
                        panel, x, y, pluginFSName, pluginFSNameIndex, isDetachedFS);

    // create the menu
    char** strings;
    static char buffShowInPanel[] = "&Show in Panel";
    static char buffDisconnect[] = "&Disconnect";
    char* strings1[] = {buffShowInPanel, buffDisconnect, NULL}; // entries for a detached plugin FS
    static char buffRefresh[] = "&Refresh";
    char* strings2[] = {buffRefresh, buffDisconnect, NULL}; // entries for an active plugin FS
    static char buffConnect[] = "Connect To...";
    char* strings3[] = {buffConnect, NULL}; // entries for the FS
    if (pluginFS != NULL)
    {
        if (isDetachedFS)
            strings = strings1;
        else
            strings = strings2;
    }
    else
        strings = strings3;

    HMENU menu = CreatePopupMenu();
    MENUITEMINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_TYPE | MIIM_ID;
    mi.fType = MFT_STRING;
    int i;
    for (i = 0; strings[i] != NULL; i++)
    {
        char* p = strings[i];
        mi.wID = i + 1;
        mi.dwTypeData = p;
        mi.cch = (UINT)strlen(p);
        InsertMenuItem(menu, i, TRUE, &mi);
    }
    DWORD cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                 x, y, parent, NULL);
    DestroyMenu(menu);
    if (cmd != 0) // the user selected a command from the menu
    {
        refreshMenu = FALSE;
        closeMenu = FALSE;
        postCmd = 0;
        postCmdParam = NULL;

        if (pluginFS != NULL)
        {
            if (isDetachedFS) // entry for a detached plugin FS
            {
                switch (cmd)
                {
                case 1: // show in panel
                {
                    closeMenu = TRUE;
                    postCmd = 1;
                    postCmdParam = (void*)pluginFS;
                    break;
                }

                case 2: // disconnect
                {
                    // if the FS opens windows in the ReleaseObject method (not the case here),
                    // CloseDetachedFS should be called only after the change-drive menu closes
                    SalamanderGeneral->CloseDetachedFS(parent, pluginFS);
                    refreshMenu = TRUE; // if it closed, refresh the Change Drive menu
                    break;
                }
                }
            }
            else // entry for an active plugin FS
            {
                switch (cmd)
                {
                case 1: // refresh
                {
                    closeMenu = TRUE;
                    postCmd = 2;
                    break;
                }

                case 2: // disconnect
                {
                    closeMenu = TRUE;
                    postCmd = 4;
                    break;
                }
                }
            }
        }
        else // entry for the FS
        {
            switch (cmd)
            {
            case 1: // connect to...
            {
                closeMenu = TRUE;
                postCmd = 3;
                break;
            }
            }
        }
        return TRUE;
    }
    else
        return FALSE; // cancel or menu error
}

void WINAPI
CPluginInterfaceForFS::ExecuteChangeDrivePostCommand(int panel, int postCmd, void* postCmdParam)
{
    CALL_STACK_MESSAGE3("CPluginInterfaceForFS::ExecuteChangeDrivePostCommand(%d, %d,)", panel, postCmd);

    if (postCmd == 1) // entry for a detached FS: show in panel
    {
        SalamanderGeneral->ChangePanelPathToDetachedFS(panel,
                                                       (CPluginFSInterfaceAbstract*)postCmdParam,
                                                       NULL);
    }

    if (postCmd == 2) // entry for an active FS: refresh
    {
        SalamanderGeneral->RefreshPanelPath(panel);
    }

    if (postCmd == 3) // entry for the FS: connect to...
    {
        ExecuteChangeDriveMenuItem(panel);
    }

    if (postCmd == 4) // entry for an active FS: disconnect
    {                 // use PostMenuExtCommand so the disconnect runs later in "sal-idle"
        int leftOrRightPanel = panel == PANEL_SOURCE ? SalamanderGeneral->GetSourcePanel() : panel;
        SalamanderGeneral->PostMenuExtCommand(leftOrRightPanel == PANEL_LEFT ? MENUCMD_DISCONNECT_LEFT : MENUCMD_DISCONNECT_RIGHT,
                                              TRUE); // example of working with the 'panel' panel (if the command comes from the Drive bar)

        //    SalamanderGeneral->PostMenuExtCommand(MENUCMD_DISCONNECT_ACTIVE, TRUE);  // this command cannot be triggered from the Drive bar, so this implementation is sufficient
    }
}

BOOL WINAPI
CPluginInterfaceForFS::DisconnectFS(HWND parent, BOOL isInPanel, int panel,
                                    CPluginFSInterfaceAbstract* pluginFS,
                                    const char* pluginFSName, int pluginFSNameIndex)
{
    CALL_STACK_MESSAGE5("CPluginInterfaceForFS::DisconnectFS(, %d, %d, , %s, %d)",
                        isInPanel, panel, pluginFSName, pluginFSNameIndex);
    ((CPluginFSInterface*)pluginFS)->CalledFromDisconnectDialog = TRUE; // suppress unnecessary prompts (the user requested a disconnect, just perform it)
    BOOL ret = FALSE;
    if (isInPanel)
    {
        SalamanderGeneral->DisconnectFSFromPanel(parent, panel);
        ret = SalamanderGeneral->GetPanelPluginFS(panel) != pluginFS;
    }
    else
    {
        ret = SalamanderGeneral->CloseDetachedFS(parent, pluginFS);
    }
    if (!ret)
        ((CPluginFSInterface*)pluginFS)->CalledFromDisconnectDialog = FALSE; // disable the suppression of unnecessary prompts
    return ret;
}

void WINAPI
CPluginInterfaceForFS::ExecuteOnFS(int panel, CPluginFSInterfaceAbstract* pluginFS,
                                   const char* pluginFSName, int pluginFSNameIndex,
                                   CFileData& file, int isDir)
{
    CPluginFSInterface* fs = (CPluginFSInterface*)pluginFS;
    if (isDir) // podadresář nebo up-dir
    {
        char newPath[MAX_PATH];
        if (isDir == 2) // nadřazený adresář
        {
            SftpParent(fs->Path, newPath, MAX_PATH);
            fs = NULL; // po ChangePanelPathToXXX už ukazatel nemusí být platný
            SalamanderGeneral->ChangePanelPathToPluginFS(panel, pluginFSName, newPath);
        }
        else // podadresář
        {
            SftpJoin(fs->Path, file.Name, newPath, MAX_PATH);
            fs = NULL;
            SalamanderGeneral->ChangePanelPathToPluginFS(panel, pluginFSName, newPath);
        }
    }
    else // soubor: stáhni do temp a otevři přidruženou aplikací
    {
        SalamanderGeneral->SetUserWorkedOnPanelPath(panel);
        char remote[MAX_PATH];
        SftpJoin(fs->Path, file.Name, remote, MAX_PATH);
        char tmpDir[MAX_PATH], tmpFile[MAX_PATH];
        GetTempPath(MAX_PATH, tmpDir);
        lstrcpyn(tmpFile, tmpDir, MAX_PATH);
        SalamanderGeneral->SalPathAppend(tmpFile, file.Name, MAX_PATH);
        HWND parent = SalamanderGeneral->GetMsgBoxParent();
        if (SftpEnsureConnected(parent) && SftpConn.Download(remote, tmpFile))
            ShellExecute(SalamanderGeneral->GetMainWindowHWND(), "open", tmpFile, NULL, tmpDir, SW_SHOWNORMAL);
        else
        {
            char msg[600];
            _snprintf_s(msg, _TRUNCATE, "Nelze stáhnout soubor:\n%s", SftpConn.LastError());
            SalamanderGeneral->SalMessageBox(parent, msg, LoadStr(IDS_PLUGINNAME), MB_OK | MB_ICONEXCLAMATION);
        }
    }
}

//****************************************************************************
//
// CTopIndexMem
//

void CTopIndexMem::Push(const char* path, int topIndex)
{
    // determine whether path follows Path (path == Path+"\\name")
    const char* s = path + strlen(path);
    if (s > path && *(s - 1) == '\\')
        s--;
    BOOL ok;
    if (s == path)
        ok = FALSE;
    else
    {
        if (s > path && *s == '\\')
            s--;
        while (s > path && *s != '\\')
            s--;

        int l = (int)strlen(Path);
        if (l > 0 && Path[l - 1] == '\\')
            l--;
        ok = s - path == l && SalamanderGeneral->StrNICmp(path, Path, l) == 0;
    }

    if (ok) // path follows -> remember the next top index
    {
        if (TopIndexesCount == TOP_INDEX_MEM_SIZE) // discard the first stored top index
        {
            int i;
            for (i = 0; i < TOP_INDEX_MEM_SIZE - 1; i++)
                TopIndexes[i] = TopIndexes[i + 1];
            TopIndexesCount--;
        }
        strcpy(Path, path);
        TopIndexes[TopIndexesCount++] = topIndex;
    }
    else // does not follow -> first top index in the sequence
    {
        strcpy(Path, path);
        TopIndexesCount = 1;
        TopIndexes[0] = topIndex;
    }
}

BOOL CTopIndexMem::FindAndPop(const char* path, int& topIndex)
{
    // determine whether path matches Path (path == Path)
    int l1 = (int)strlen(path);
    if (l1 > 0 && path[l1 - 1] == '\\')
        l1--;
    int l2 = (int)strlen(Path);
    if (l2 > 0 && Path[l2 - 1] == '\\')
        l2--;
    if (l1 == l2 && SalamanderGeneral->StrNICmp(path, Path, l1) == 0)
    {
        if (TopIndexesCount > 0)
        {
            char* s = Path + strlen(Path);
            if (s > Path && *(s - 1) == '\\')
                s--;
            if (s > Path && *s == '\\')
                s--;
            while (s > Path && *s != '\\')
                s--;
            *s = 0;
            topIndex = TopIndexes[--TopIndexesCount];
            return TRUE;
        }
        else // we no longer have this value (it was not stored or was discarded due to low memory)
        {
            Clear();
            return FALSE;
        }
    }
    else // request for a different path -> clear memory; a long jump occurred
    {
        Clear();
        return FALSE;
    }
}

//
// ****************************************************************************
// CPluginFSDataInterface
//

CPluginFSDataInterface::CPluginFSDataInterface(const char* path)
{
    strcpy(Path, path);
    SalamanderGeneral->SalPathAddBackslash(Path, MAX_PATH);
    Name = Path + strlen(Path);
}

HIMAGELIST WINAPI
CPluginFSDataInterface::GetSimplePluginIcons(int iconSize)
{
    return DFSImageList;
}

HICON WINAPI
CPluginFSDataInterface::GetPluginIcon(const CFileData* file, int iconSize, BOOL& destroyIcon)
{
    lstrcpyn(Name, file->Name, (int)(MAX_PATH - (Name - Path)));
    HICON icon;
    if (!SalamanderGeneral->GetFileIcon(Path, FALSE, &icon, iconSize, FALSE, FALSE))
        icon = NULL;
    destroyIcon = TRUE;
    return icon; // icon or NULL (failure)
}

// globální proměnná pro "get text" callbacky sloupců
CFSData* FSdata;

// callbacky volané Salamanderem pro text vlastních sloupců (viz spl_com.h / FColumnGetText)
void WINAPI GetRightsText()
{
    FSdata = (CFSData*)((*TransferFileData)->PluginData);
    memcpy(TransferBuffer, FSdata->Rights, (*TransferLen = (int)strlen(FSdata->Rights)));
}

void WINAPI GetOwnerText()
{
    FSdata = (CFSData*)((*TransferFileData)->PluginData);
    memcpy(TransferBuffer, FSdata->Owner, (*TransferLen = (int)strlen(FSdata->Owner)));
}

void WINAPI GetGroupText()
{
    FSdata = (CFSData*)((*TransferFileData)->PluginData);
    memcpy(TransferBuffer, FSdata->Group, (*TransferLen = (int)strlen(FSdata->Group)));
}

int WINAPI PluginSimpleIconCallback()
{
    return *TransferIsDir ? 0 : 1;
}

void AddRightsColumns(BOOL leftPanel, CSalamanderViewAbstract* view, int& i)
{
    CColumn column;
    strcpy(column.Name, "Práva");
    strcpy(column.Description, "Přístupová práva (rwx)");
    column.GetText = GetRightsText;
    column.CustomData = 1;
    column.SupportSorting = 1;
    column.LeftAlignment = 1;
    column.ID = COLUMN_ID_CUSTOM;
    column.Width = leftPanel ? LOWORD(CreatedWidth) : HIWORD(CreatedWidth);
    column.FixedWidth = leftPanel ? LOWORD(CreatedFixedWidth) : HIWORD(CreatedFixedWidth);
    view->InsertColumn(i++, &column);

    strcpy(column.Name, "Vlastník");
    strcpy(column.Description, "Vlastník souboru");
    column.GetText = GetOwnerText;
    column.CustomData = 2;
    column.Width = leftPanel ? LOWORD(ModifiedWidth) : HIWORD(ModifiedWidth);
    column.FixedWidth = leftPanel ? LOWORD(ModifiedFixedWidth) : HIWORD(ModifiedFixedWidth);
    view->InsertColumn(i++, &column);

    strcpy(column.Name, "Skupina");
    strcpy(column.Description, "Skupina souboru");
    column.GetText = GetGroupText;
    column.CustomData = 3;
    column.Width = leftPanel ? LOWORD(AccessedWidth) : HIWORD(AccessedWidth);
    column.FixedWidth = leftPanel ? LOWORD(AccessedFixedWidth) : HIWORD(AccessedFixedWidth);
    view->InsertColumn(i++, &column);
}

void WINAPI
CPluginFSDataInterface::SetupView(BOOL leftPanel, CSalamanderViewAbstract* view, const char* archivePath,
                                  const CFileData* upperDir)
{
    view->GetTransferVariables(TransferFileData, TransferIsDir, TransferBuffer, TransferLen, TransferRowData,
                               TransferPluginDataIface, TransferActCustomData);

    view->SetPluginSimpleIconCallback(PluginSimpleIconCallback);

    // v detailním zobrazení přidej sloupce Práva / Vlastník / Skupina (jako WinSCP)
    if (view->GetViewMode() == VIEW_MODE_DETAILED)
    {
        int count = view->GetColumnsCount();
        AddRightsColumns(leftPanel, view, count);
    }
}

void WINAPI
CPluginFSDataInterface::ColumnFixedWidthShouldChange(BOOL leftPanel, const CColumn* column, int newFixedWidth)
{
    if (leftPanel)
    {
        switch (column->CustomData)
        {
        case 1:
            CreatedFixedWidth = MAKELONG(newFixedWidth, HIWORD(CreatedFixedWidth));
            break;
        case 2:
            ModifiedFixedWidth = MAKELONG(newFixedWidth, HIWORD(ModifiedFixedWidth));
            break;
        case 3:
            AccessedFixedWidth = MAKELONG(newFixedWidth, HIWORD(AccessedFixedWidth));
            break;
        case 4:
            DFSTypeFixedWidth = MAKELONG(newFixedWidth, HIWORD(DFSTypeFixedWidth));
            break;
        }
    }
    else
    {
        switch (column->CustomData)
        {
        case 1:
            CreatedFixedWidth = MAKELONG(LOWORD(CreatedFixedWidth), newFixedWidth);
            break;
        case 2:
            ModifiedFixedWidth = MAKELONG(LOWORD(ModifiedFixedWidth), newFixedWidth);
            break;
        case 3:
            AccessedFixedWidth = MAKELONG(LOWORD(AccessedFixedWidth), newFixedWidth);
            break;
        case 4:
            DFSTypeFixedWidth = MAKELONG(LOWORD(DFSTypeFixedWidth), newFixedWidth);
            break;
        }
    }
    if (newFixedWidth)
        ColumnWidthWasChanged(leftPanel, column, column->Width);
}

void WINAPI
CPluginFSDataInterface::ColumnWidthWasChanged(BOOL leftPanel, const CColumn* column, int newWidth)
{
    if (leftPanel)
    {
        switch (column->CustomData)
        {
        case 1:
            CreatedWidth = MAKELONG(newWidth, HIWORD(CreatedWidth));
            break;
        case 2:
            ModifiedWidth = MAKELONG(newWidth, HIWORD(ModifiedWidth));
            break;
        case 3:
            AccessedWidth = MAKELONG(newWidth, HIWORD(AccessedWidth));
            break;
        case 4:
            DFSTypeWidth = MAKELONG(newWidth, HIWORD(DFSTypeWidth));
            break;
        }
    }
    else
    {
        switch (column->CustomData)
        {
        case 1:
            CreatedWidth = MAKELONG(LOWORD(CreatedWidth), newWidth);
            break;
        case 2:
            ModifiedWidth = MAKELONG(LOWORD(ModifiedWidth), newWidth);
            break;
        case 3:
            AccessedWidth = MAKELONG(LOWORD(AccessedWidth), newWidth);
            break;
        case 4:
            DFSTypeWidth = MAKELONG(LOWORD(DFSTypeWidth), newWidth);
            break;
        }
    }
}

struct CFSInfoLineData
{
    const char* Name;
    const char* Type;
};

const char* WINAPI FSInfoLineFile(HWND parent, void* param)
{
    CFSInfoLineData* data = (CFSInfoLineData*)param;
    return data->Name;
}

const char* WINAPI FSInfoLineType(HWND parent, void* param)
{
    CFSInfoLineData* data = (CFSInfoLineData*)param;
    return data->Type;
}

CSalamanderVarStrEntry FSInfoLine[] =
    {
        {"File", FSInfoLineFile},
        {"Type", FSInfoLineType},
        {NULL, NULL}};

BOOL WINAPI
CPluginFSDataInterface::GetInfoLineContent(int panel, const CFileData* file, BOOL isDir,
                                           int selectedFiles, int selectedDirs, BOOL displaySize,
                                           const CQuadWord& selectedSize, char* buffer,
                                           DWORD* hotTexts, int& hotTextsCount)
{
    if (file != NULL)
    {
        CFSInfoLineData data;
        data.Name = file->Name;
        data.Type = ((CFSData*)file->PluginData)->Rights;
        hotTextsCount = 100;
        if (!SalamanderGeneral->ExpandVarString(SalamanderGeneral->GetMsgBoxParent(),
                                                "$(File): $(Type)", buffer, 1000, FSInfoLine,
                                                &data, FALSE, hotTexts, &hotTextsCount))
        {
            strcpy(buffer, "Error!");
            hotTextsCount = 0;
        }
        return TRUE;
    }
    else
    {
        if (selectedFiles == 0 && selectedDirs == 0) // Information Line for an empty panel
        {
            // return FALSE;  // let Salamander print the text
            strcpy(buffer, "No items found");
            hotTextsCount = 0;
            return TRUE;
        }
        // return FALSE;  // let Salamander print the counts of selected files and directories
        if (displaySize)
        {
            /*
      // double-check the sum
      CQuadWord mySize(0, 0);
      int index = 0;
      const CFileData *file = NULL;
      while ((file = SalamanderGeneral->GetPanelSelectedItem(panel, &index, NULL)) != NULL)
      {
        mySize += file->Size;
      }
      if (mySize != selectedSize) TRACE_E("Unexpected situation in CPluginFSDataInterface::GetInfoLineContent().");
*/

            char num[100];
            SalamanderGeneral->PrintDiskSize(num, selectedSize, 0);
            // for simplicity we do not use "plural" strings (see SalamanderGeneral->ExpandPluralString())
            sprintf(buffer, "Selected (<%s>): <%d> files and <%d> directories", num, selectedFiles, selectedDirs);

            /*    // example of using a standard string
      SalamanderGeneral->ExpandPluralBytesFilesDirs(buffer, 1000, selectedSize, selectedFiles,
                                                    selectedDirs, TRUE);
*/
        }
        else
            sprintf(buffer, "Selected: <%d> files and <%d> directories", selectedFiles, selectedDirs);
        return SalamanderGeneral->LookForSubTexts(buffer, hotTexts, &hotTextsCount);
    }
}

//
// ****************************************************************************
// CFSData
//

