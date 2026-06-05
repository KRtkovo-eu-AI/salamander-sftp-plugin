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

// plugin interface object whose methods are invoked by Salamander
CPluginInterface PluginInterface;
// additional parts of the CPluginInterface implementation
CPluginInterfaceForArchiver InterfaceForArchiver;
CPluginInterfaceForViewer InterfaceForViewer;
CPluginInterfaceForMenuExt InterfaceForMenuExt;
CPluginInterfaceForFS InterfaceForFS;
CPluginInterfaceForThumbLoader InterfaceForThumbLoader;

// global data
const char* PluginNameEN = "SFTP";    // untranslated plugin name, used before the language module loads and for debugging
const char* PluginNameShort = "SFTP"; // plugin name (short form, without spaces)

char Str[MAX_PATH] = "default";
int Number = 0;
int Selection = 1; // "second" in configuration dialog
BOOL CheckBox = FALSE;
int RadioBox = 13;                        // radio 2 in configuration dialog
BOOL CfgSavePosition = FALSE;             // store the window position / align to the main window
WINDOWPLACEMENT CfgWindowPlacement = {0}; // invalid unless CfgSavePosition == TRUE
int Size2FixedWidth = 0;                  // Size2 column (archiver): low/high word -> left/right panel: FixedWidth
int Size2Width = 0;                       // Size2 column (archiver): low/high word -> left/right panel: Width
int CreatedFixedWidth = 0;                // Created column (FS): low/high word -> left/right panel: FixedWidth
int CreatedWidth = 0;                     // Created column (FS): low/high word -> left/right panel: Width
int ModifiedFixedWidth = 0;               // Modified column (FS): low/high word -> left/right panel: FixedWidth
int ModifiedWidth = 0;                    // Modified column (FS): low/high word -> left/right panel: Width
int AccessedFixedWidth = 0;               // Accessed column (FS): low/high word -> left/right panel: FixedWidth
int AccessedWidth = 0;                    // Accessed column (FS): low/high word -> left/right panel: Width
int DFSTypeFixedWidth = 0;                // DFS Type column (FS): low/high word -> left/right panel: FixedWidth
int DFSTypeWidth = 0;                     // DFS Type column (FS): low/high word -> left/right panel: Width

DWORD LastCfgPage = 0; // start page (sheet) in configuration dialog

const char* CONFIG_KEY = "test key";
const char* CONFIG_STR = "test string";
const char* CONFIG_NUMBER = "test number";
const char* CONFIG_SAVEPOS = "SavePosition";
const char* CONFIG_WNDPLACEMENT = "WindowPlacement";
const char* CONFIG_SIZE2FIXEDWIDTH = "Size2FixedWidth";
const char* CONFIG_SIZE2WIDTH = "Size2Width";
const char* CONFIG_CREATEDFIXEDWIDTH = "CreatedFixedWidth";
const char* CONFIG_CREATEDWIDTH = "CreatedWidth";
const char* CONFIG_MODIFIEDFIXEDWIDTH = "ModifiedFixedWidth";
const char* CONFIG_MODIFIEDWIDTH = "ModifiedWidth";
const char* CONFIG_ACCESSEDFIXEDWIDTH = "AccessedFixedWidth";
const char* CONFIG_ACCESSEDWIDTH = "AccessedWidth";
const char* CONFIG_DFSTYPEFIXEDWIDTH = "DFSTypeFixedWidth";
const char* CONFIG_DFSTYPEWIDTH = "DFSTypeWidth";

// ConfigVersion: 0 - no configuration was loaded from the registry (fresh plugin installation),
//                1 - first configuration version
//                2 - second configuration version (some configuration values were added)
//                3 - third configuration version (extension "dmp2" added)
//                4 - fourth configuration version (changed extension "dmp" (collides with minidumps) to "dop" and "dmp2" to "dop2")

int ConfigVersion = 0;           // version of the configuration loaded from the registry (see the descriptions above)
#define CURRENT_CONFIG_VERSION 4 // current configuration version (stored in the registry when the plugin unloads)
const char* CONFIG_VERSION = "Version";

// pointers to lowercase/uppercase mapping tables
unsigned char* LowerCase = NULL;
unsigned char* UpperCase = NULL;

HINSTANCE DLLInstance = NULL; // handle to the SPL - language-independent resources
HINSTANCE HLanguage = NULL;   // handle to the SLG - language-dependent resources

// Salamander general interface - valid from startup until the plugin shuts down
CSalamanderGeneralAbstract* SalamanderGeneral = NULL;

// variable declaration for "dbg.h"
CSalamanderDebugAbstract* SalamanderDebug = NULL;

// variable declaration for "spl_com.h"
int SalamanderVersion = 0;

// interface that provides customized Windows controls used in Salamander
CSalamanderGUIAbstract* SalamanderGUI = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DLLInstance = hinstDLL;

        INITCOMMONCONTROLSEX initCtrls;
        initCtrls.dwSize = sizeof(INITCOMMONCONTROLSEX);
        initCtrls.dwICC = ICC_BAR_CLASSES;
        if (!InitCommonControlsEx(&initCtrls))
        {
            MessageBox(NULL, "InitCommonControlsEx failed!", "Error", MB_OK | MB_ICONERROR);
            return FALSE; // DLL won't start
        }
    }

    return TRUE; // DLL can be loaded
}

char* LoadStr(int resID)
{
    return SalamanderGeneral->LoadStr(HLanguage, resID);
}

void OnConfiguration(HWND hParent)
{
    static BOOL InConfiguration = FALSE;
    if (InConfiguration)
    {
        SalamanderGeneral->SalMessageBox(hParent, LoadStr(IDS_CFG_ALREADY_OPENED), LoadStr(IDS_PLUGINNAME),
                                         MB_ICONINFORMATION | MB_OK);
        return;
    }
    InConfiguration = TRUE;
    if (CConfigDialog(hParent).Execute() == IDOK)
    {
        ViewerWindowQueue.BroadcastMessage(WM_USER_VIEWERCFGCHNG, 0, 0);
    }
    InConfiguration = FALSE;
}

void OnAbout(HWND hParent)
{
    char buf[1000];
    _snprintf_s(buf, _TRUNCATE,
                "%s " VERSINFO_VERSION "\n\n" VERSINFO_COPYRIGHT "\n\n"
                "%s",
                LoadStr(IDS_PLUGINNAME),
                LoadStr(IDS_PLUGIN_DESCRIPTION));
    SalamanderGeneral->SalMessageBox(hParent, buf, LoadStr(IDS_ABOUT), MB_OK | MB_ICONINFORMATION);
}

void InitIconOverlays()
{
    // clear the currently registered icon overlays so we stay clean even if an error occurs
    SalamanderGeneral->SetPluginIconOverlays(0, NULL);

    HINSTANCE imageResDLL = HANDLES(LoadLibraryEx("imageres.dll", NULL, LOAD_LIBRARY_AS_DATAFILE));
    // Salamander does not run without imageres.dll, so loading it here cannot fail

    // 48x48 only from XP onward (soon obsolete, we will run only on XP+, then drop it)
    // in reality large icons have been supported for a long time and can be enabled via
    // Desktop/Properties/???/Large Icons; note that there will be no system image list
    // for 32x32 icons in that case, and we should fetch the actual icon sizes from the system
    // for now we ignore this and enable 48x48 only from XP, where they are normally available
    int iconSizes[3] = {16, 32, 48};
    if (!SalIsWindowsVersionOrGreater(5, 1, 0)) // not WindowsXPAndLater: not XP or later
        iconSizes[2] = 32;

    HINSTANCE iconDLL = imageResDLL;
    int iconOverlaysCount = 0;
    HICON iconOverlays[6];
    int iconIndex = 164; // shared icon-overlay
    for (int i = 0; i < 3; i++)
    {
        iconOverlays[iconOverlaysCount++] = (HICON)LoadImage(iconDLL, MAKEINTRESOURCE(iconIndex),
                                                             IMAGE_ICON, iconSizes[i], iconSizes[i],
                                                             SalamanderGeneral->GetIconLRFlags());
    }

    iconIndex = 97; // slow file icon-overlay
    for (int i = 0; i < 3; i++)
    {
        iconOverlays[iconOverlaysCount++] = (HICON)LoadImage(iconDLL, MAKEINTRESOURCE(iconIndex),
                                                             IMAGE_ICON, iconSizes[i], iconSizes[i],
                                                             SalamanderGeneral->GetIconLRFlags());
    }

    // register two overlays: shared + slow file
    // NOTE: if icon loading fails, SetPluginIconOverlays() fails but releases the valid icons in iconOverlays[]
    SalamanderGeneral->SetPluginIconOverlays(iconOverlaysCount / 3, iconOverlays);

    if (imageResDLL != NULL)
        HANDLES(FreeLibrary(imageResDLL));
}

//
// ****************************************************************************
// SalamanderPluginGetReqVer and SalamanderPluginGetSDKVer
//

#ifdef __BORLANDC__
extern "C"
{
    int WINAPI SalamanderPluginGetReqVer();
    int WINAPI SalamanderPluginGetSDKVer();
    CPluginInterfaceAbstract* WINAPI SalamanderPluginEntry(CSalamanderPluginEntryAbstract* salamander);
};
#endif // __BORLANDC__

int WINAPI SalamanderPluginGetReqVer()
{
#ifdef SFTP_COMPATIBLE_WITH_500
    return 103; // plugin works in Open Salamander 5.0 or later
#else           // SFTP_COMPATIBLE_WITH_500
    return LAST_VERSION_OF_SALAMANDER;
#endif          // SFTP_COMPATIBLE_WITH_500
}

int WINAPI SalamanderPluginGetSDKVer()
{
    return LAST_VERSION_OF_SALAMANDER; // return current SDK version
}

//
// ****************************************************************************
// SalamanderPluginEntry
//

/*
void WINAPI TestLoadOrSaveConfiguration(BOOL load, HKEY regKey,
                                        CSalamanderRegistryAbstract *registry, void *param)
{
  CALL_STACK_MESSAGE2("TestLoadOrSaveConfiguration(%d, ,)", load);
  char buf[MAX_PATH];
  if (load)  // load
  {
    // load default configuration
    buf[0] = 0;

    if (regKey != NULL)   // load from the registry
    {
      HKEY actKey;
      if (registry->OpenKey(regKey, "test-key", actKey))
      {
        if (!registry->GetValue(actKey, "test-value", REG_SZ, buf, MAX_PATH)) buf[0] = 0;
        registry->CloseKey(actKey);
      }
    }
  }
  else  // save
  {
    strcpy(buf, "test-string");
    HKEY actKey;
    if (registry->CreateKey(regKey, "test-key", actKey))
    {
      registry->SetValue(actKey, "test-value", REG_SZ, buf, -1);
      registry->CloseKey(actKey);
    }
  }
}
*/

/*
struct VS_VERSIONINFO_HEADER
{
  WORD  wLength;
  WORD  wValueLength;
  WORD  wType;
};

BOOL GetModuleVersion(HINSTANCE hModule, char *buffer, int bufferLen)
{
  HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
  if (hRes == NULL)
  {
    lstrcpyn(buffer, "unknown", bufferLen);
    return FALSE;
  }

  HGLOBAL hVer = LoadResource(hModule, hRes);
  if (hVer == NULL)
  {
    lstrcpyn(buffer, "unknown", bufferLen);
    return FALSE;
  }

  DWORD resSize = SizeofResource(hModule, hRes);
  const BYTE *first = (BYTE*)LockResource(hVer);
  if (resSize == 0 || first == 0)
  {
    lstrcpyn(buffer, "unknown", bufferLen);
    return FALSE;
  }
  const BYTE *iterator = first + sizeof(VS_VERSIONINFO_HEADER);

  DWORD signature = 0xFEEF04BD;

  while (memcmp(iterator, &signature, 4) != 0)
  {
    iterator++;
    if (iterator + 4 >= first + resSize)
    {
      lstrcpyn(buffer, "unknown", bufferLen);
      return FALSE;
    }
  }

  VS_FIXEDFILEINFO *ffi = (VS_FIXEDFILEINFO *)iterator;

  char buff[200];
  sprintf(buff, "%d.%d.%d.%d", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
           HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
  lstrcpyn(buffer, buff, bufferLen);
  return TRUE;
}
*/
/*

BOOL GetVerInfo(const char *fileName)
{
   BOOL bResult = FALSE;
   DWORD  handle;
   UINT  uiInfoSize;
   UINT  uiVerSize ;
   UINT  uiSize ;
   BYTE* pbData = NULL ;
   DWORD* lpBuffer;;
   char szName[512] ;

   // Get the size of the version information.
   uiInfoSize = ::GetFileVersionInfoSize(fileName, &handle);
   if (uiInfoSize == 0) return FALSE ;

   // Allocate a buffer for the version information.
   pbData = new BYTE[uiInfoSize] ;

   // Fill the buffer with the version information.
   bResult = ::GetFileVersionInfo(fileName, handle, uiInfoSize, pbData);
   if (!bResult) goto NastyGoto ;

   // Get the translation information.
   bResult = 
     ::VerQueryValue( pbData,
                      "\\VarFileInfo\\Translation",
                      (void**)&lpBuffer,
                      &uiVerSize);
   if (!bResult) goto NastyGoto ;

   bResult = uiVerSize ;
   if (!bResult) goto NastyGoto ;

   // Build the path to the OLESelfRegister key
   // using the translation information.
   sprintf( szName,
             "\\StringFileInfo\\%04hX%04hX\\OLESelfRegister",
             LOWORD(*lpBuffer),
             HIWORD(*lpBuffer)) ;

   // Search for the key.
   bResult = ::VerQueryValue( pbData, 
                              szName, 
                              (void**)&lpBuffer, 
                              &uiSize);

NastyGoto:
   delete [] pbData ;
   return bResult ;
}


*/
CPluginInterfaceAbstract* WINAPI SalamanderPluginEntry(CSalamanderPluginEntryAbstract* salamander)
{
    // set SalamanderDebug for "dbg.h"
    SalamanderDebug = salamander->GetSalamanderDebug();
    // set SalamanderVersion for "spl_com.h"
    SalamanderVersion = salamander->GetVersion();

    HANDLES_CAN_USE_TRACE();
    CALL_STACK_MESSAGE1("SalamanderPluginEntry()");

    //  char path[MAX_PATH];
    //  GetModuleFileName(DLLInstance, path, MAX_PATH);
    //  GetVerInfo(path, path, MAX_PATH);

    // this plugin targets the current Salamander version and newer - perform a check
#ifdef SFTP_COMPATIBLE_WITH_500
    if (SalamanderVersion < 103) // plugin works in Open Salamander 5.0 or later
#else                            // SFTP_COMPATIBLE_WITH_500
    if (SalamanderVersion < LAST_VERSION_OF_SALAMANDER)
#endif                           // SFTP_COMPATIBLE_WITH_500
    {                            // reject older versions
        MessageBox(salamander->GetParentWindow(),
#ifdef SFTP_COMPATIBLE_WITH_500
                   "This plugin requires Open Salamander 5.0 or later.",
#else  // SFTP_COMPATIBLE_WITH_500
                   REQUIRE_LAST_VERSION_OF_SALAMANDER,
#endif // SFTP_COMPATIBLE_WITH_500
                   PluginNameEN, MB_OK | MB_ICONERROR);
        return NULL;
    }

    // load the language module (.slg)
    HLanguage = salamander->LoadLanguageModule(salamander->GetParentWindow(), PluginNameEN);
    if (HLanguage == NULL)
        return NULL;

    // obtain the general Salamander interface
    SalamanderGeneral = salamander->GetSalamanderGeneral();
    SalamanderGeneral->GetLowerAndUpperCase(&LowerCase, &UpperCase);
    // obtain the interface providing customized Windows controls used in Salamander
    SalamanderGUI = salamander->GetSalamanderGUI();

    // configure the help file name
    SalamanderGeneral->SetHelpFileName("sftp.chm");

    //  BYTE c = SalamanderGeneral->GetUserDefaultCharset();

    if (!InitViewer())
        return NULL; // error

    if (!InitFS())
    {
        ReleaseViewer();
        return NULL; // error
    }

    InitIconOverlays();

    /*
  // demonstration of an application crash
  int *p = 0;
  *p = 0;       // ACCESS VIOLATION !
*/

#ifndef SFTP_QUIET
    // display a test notification
    char buf[100];
    sprintf(buf, "SalamanderPluginEntry called, Salamander version %d", salamander->GetVersion());
    TRACE_I(buf); // into TRACE SERVER
    SalamanderGeneral->SalMessageBox(salamander->GetParentWindow(), buf, LoadStr(IDS_PLUGINNAME),
                                     MB_OK | MB_ICONINFORMATION); // into a window
#endif                                                            // SFTP_QUIET

    // provide basic information about the plugin
    salamander->SetBasicPluginData(LoadStr(IDS_PLUGINNAME),
                                   FUNCTION_LOADSAVECONFIGURATION |
                                       FUNCTION_FILESYSTEM,
                                   VERSINFO_VERSION_NO_PLATFORM, VERSINFO_COPYRIGHT, LoadStr(IDS_PLUGIN_DESCRIPTION),
                                   PluginNameShort, NULL, "dfs");

    // request notifications about master password creation, changes, and removal
    SalamanderGeneral->SetPluginUsesPasswordManager();

    // set the plugin home page URL
    salamander->SetPluginHomePageURL(LoadStr(IDS_PLUGIN_HOME));

    // obtain our FS name (it might not stay "dfs" because Salamander can adjust it)
    SalamanderGeneral->GetPluginFSName(AssignedFSName, 0);
    AssignedFSNameLen = (int)strlen(AssignedFSName);

    // test adding multiple filesystem names
    char demoFSAssignedFSName[MAX_PATH]; // should live in a global variable (so it can be used throughout the plugin)
    demoFSAssignedFSName[0] = 0;
    int demoFSFSNameIndex; // should live in a global variable (so it can be used throughout the plugin)
    if (salamander->AddFSName("demofs", &demoFSFSNameIndex))
        SalamanderGeneral->GetPluginFSName(demoFSAssignedFSName, demoFSFSNameIndex);
    char demoAssignedFSName[MAX_PATH]; // should live in a global variable (so it can be used throughout the plugin)
    demoAssignedFSName[0] = 0;
    int demoFSNameIndex; // should live in a global variable (so it can be used throughout the plugin)
    if (salamander->AddFSName("demo", &demoFSNameIndex))
        SalamanderGeneral->GetPluginFSName(demoAssignedFSName, demoFSNameIndex);

    // test module enumeration
    /*
  int index = 0;
  char module[MAX_PATH];
  char version[MAX_PATH];
  while (SalamanderGeneral->EnumInstalledModules(&index, module, version))
  {
    TRACE_I("Module " << module << ", version " << version);
  }
*/
    // test CallLoadOrSaveConfiguration
    //  SalamanderGeneral->CallLoadOrSaveConfiguration(TRUE, TestLoadOrSaveConfiguration, (void *)0x1234);

    // test CopyTextToClipboard
    //  BOOL success = SalamanderGeneral->CopyTextToClipboard("testíček", -1,
    //                                                        TRUE, salamander->GetParentWindow());

    // test CopyTextToClipboardW
    //  BOOL success = SalamanderGeneral->CopyTextToClipboardW(L"testíček", -1,
    //                                                         TRUE, salamander->GetParentWindow());

    // test SetPluginBugReportInfo
    //  SalamanderGeneral->SetPluginBugReportInfo(LoadStr(IDS_PLUGIN_BUGREP), LoadStr(IDS_PLUGIN_EMAIL));
    /*
  int gcp_type;
  BOOL gcp_val;
  char gcp_val2[300];
  BOOL gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_SELOPINCLUDEDIRS, &gcp_val,
                                                       sizeof(gcp_val), &gcp_type);
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_INFOLINECONTENT, &gcp_val,
                                                  sizeof(gcp_val), &gcp_type);
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_INFOLINECONTENT, gcp_val2, 300, &gcp_type);
  int gcp_val3;
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_USERECYCLEBIN, &gcp_val3, sizeof(gcp_val3), &gcp_type);
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_COMPDIRSUSETIMERES, &gcp_val,
                                                  sizeof(gcp_val), &gcp_type);
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_COMPDIRTIMERES, &gcp_val3, sizeof(gcp_val3), NULL);
  LOGFONT lf;
  gcp_ret = SalamanderGeneral->GetConfigParameter(SALCFG_VIEWERFONT, &lf, sizeof(lf), &gcp_type);
*/
    /*
  BOOL fsItemVisible = SalamanderGeneral->GetChangeDriveMenuItemVisibility();
  if (salamander->GetLoadInformation() & LOADINFO_INSTALL) // probably better to check according to the plugin configuration version
    SalamanderGeneral->SetChangeDriveMenuItemVisibility(FALSE);
*/

    // configure the plugin to load with Salamander startup so we can clean up regularly
    // the temporary directory left by previous Sftp instances
    SalamanderGeneral->SetFlagLoadOnSalamanderStart(TRUE);

    // if the plugin was loaded because of the "load on start" flag, verify whether
    // this Salamander process is the first instance and trigger the temp-directory cleanup if so
    if (salamander->GetLoadInformation() & LOADINFO_LOADONSTART)
    {
        if (SalamanderGeneral->IsFirstInstance3OrLater())
            SalamanderGeneral->PostMenuExtCommand(MENUCMD_CHECKSFTPTMPDIR, TRUE); // delivered when Salamander becomes idle (otherwise it could not be triggered from the entry point)
        SalamanderGeneral->PostUnloadThisPlugin();                                    // after all commands are processed the plugin unloads (keeping it loaded would be pointless)
    }

    return &PluginInterface;
}

//
// ****************************************************************************
// CPluginInterface
//

void WINAPI
CPluginInterface::About(HWND parent)
{
    OnAbout(parent);
}

BOOL WINAPI
CPluginInterface::Release(HWND parent, BOOL force)
{
    CALL_STACK_MESSAGE2("CPluginInterface::Release(, %d)", force);
    BOOL ret = ViewerWindowQueue.Empty();
    if (!ret && (force || SalamanderGeneral->SalMessageBox(parent, LoadStr(IDS_VIEWER_OPENWNDS),
                                                           LoadStr(IDS_PLUGINNAME),
                                                           MB_YESNO | MB_ICONQUESTION) == IDYES))
    {
        ret = ViewerWindowQueue.CloseAllWindows(force) || force;
    }
    if (ret)
    {
        if (!ThreadQueue.KillAll(force) && !force)
            ret = FALSE;
        else
        {
            // test CallLoadOrSaveConfiguration
            //SalamanderGeneral->CallLoadOrSaveConfiguration(FALSE, TestLoadOrSaveConfiguration, (void *)0x1234);

            ReleaseViewer();
            ReleaseFS();

            // remove all filesystem file copies from the disk cache (theoretically redundant, each FS should clean its copies)
            char uniqueFileName[MAX_PATH];
            strcpy(uniqueFileName, AssignedFSName);
            strcat(uniqueFileName, ":");
            // disk names are case-insensitive while the disk cache is case-sensitive, converting
            // to lowercase makes the disk cache behave as case-insensitive as well
            SalamanderGeneral->ToLowerCase(uniqueFileName);
            SalamanderGeneral->RemoveFilesFromCache(uniqueFileName);
        }
    }
    if (ret && InterfaceForFS.GetActiveFSCount() != 0)
    {
        TRACE_E("Some FS interfaces were not closed (count=" << InterfaceForFS.GetActiveFSCount() << ")");
    }
    return ret;
}

void WINAPI
CPluginInterface::LoadConfiguration(HWND parent, HKEY regKey, CSalamanderRegistryAbstract* registry)
{
    CALL_STACK_MESSAGE1("CPluginInterface::LoadConfiguration(, ,)");

    if (regKey != NULL) // load from the registry
    {
        if (!registry->GetValue(regKey, CONFIG_VERSION, REG_DWORD, &ConfigVersion, sizeof(DWORD)))
            ConfigVersion = 1; // configuration version stored in the registry (used previously, just without a version number)

        registry->GetValue(regKey, "DefaultSession", REG_SZ, SftpDefaultSession, sizeof(SftpDefaultSession));

        HKEY actKey;
        if (registry->OpenKey(regKey, CONFIG_KEY, actKey))
        {
            registry->GetValue(actKey, CONFIG_STR, REG_SZ, Str, MAX_PATH);
            registry->CloseKey(actKey);
        }

        registry->GetValue(regKey, CONFIG_NUMBER, REG_DWORD, &Number, sizeof(DWORD));

        registry->GetValue(regKey, CONFIG_SAVEPOS, REG_DWORD, &CfgSavePosition, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_WNDPLACEMENT, REG_BINARY, &CfgWindowPlacement, sizeof(WINDOWPLACEMENT));

        registry->GetValue(regKey, CONFIG_SIZE2FIXEDWIDTH, REG_DWORD, &Size2FixedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_SIZE2WIDTH, REG_DWORD, &Size2Width, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_CREATEDFIXEDWIDTH, REG_DWORD, &CreatedFixedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_CREATEDWIDTH, REG_DWORD, &CreatedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_MODIFIEDFIXEDWIDTH, REG_DWORD, &ModifiedFixedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_MODIFIEDWIDTH, REG_DWORD, &ModifiedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_ACCESSEDFIXEDWIDTH, REG_DWORD, &AccessedFixedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_ACCESSEDWIDTH, REG_DWORD, &AccessedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_DFSTYPEFIXEDWIDTH, REG_DWORD, &DFSTypeFixedWidth, sizeof(DWORD));
        registry->GetValue(regKey, CONFIG_DFSTYPEWIDTH, REG_DWORD, &DFSTypeWidth, sizeof(DWORD));

        // načti uložené servery (profily)
        HKEY serversKey;
        if (registry->OpenKey(regKey, "Servers", serversKey))
        {
            DWORD cnt = 0;
            registry->GetValue(serversKey, "Count", REG_DWORD, &cnt, sizeof(DWORD));
            if (cnt > SFTP_MAX_PROFILES)
                cnt = SFTP_MAX_PROFILES;
            SftpProfileCount = 0;
            for (DWORD i = 0; i < cnt; i++)
            {
                char idx[16];
                sprintf(idx, "%u", i);
                HKEY pk;
                if (registry->OpenKey(serversKey, idx, pk))
                {
                    CSftpSavedProfile& p = SftpProfiles[SftpProfileCount];
                    memset(&p, 0, sizeof(p));
                    p.Port = 22;
                    registry->GetValue(pk, "Name", REG_SZ, p.Name, sizeof(p.Name));
                    registry->GetValue(pk, "Host", REG_SZ, p.Host, sizeof(p.Host));
                    DWORD port = 22;
                    registry->GetValue(pk, "Port", REG_DWORD, &port, sizeof(DWORD));
                    p.Port = (int)port;
                    registry->GetValue(pk, "User", REG_SZ, p.User, sizeof(p.User));
                    registry->GetValue(pk, "Password", REG_SZ, p.Password, sizeof(p.Password));
                    registry->GetValue(pk, "KeyFile", REG_SZ, p.KeyFile, sizeof(p.KeyFile));
                    registry->GetValue(pk, "Path", REG_SZ, p.Path, sizeof(p.Path));
                    DWORD comp = 0, proto = 0, scpfb = 0;
                    registry->GetValue(pk, "Compression", REG_DWORD, &comp, sizeof(DWORD));
                    registry->GetValue(pk, "Protocol", REG_DWORD, &proto, sizeof(DWORD));
                    registry->GetValue(pk, "ScpFallback", REG_DWORD, &scpfb, sizeof(DWORD));
                    p.UseCompression = comp != 0;
                    p.Protocol = (int)proto;
                    p.ScpFallback = scpfb != 0;
                    registry->CloseKey(pk);
                    SftpProfileCount++;
                }
            }
            registry->CloseKey(serversKey);
        }
    }
}

void WINAPI
CPluginInterface::SaveConfiguration(HWND parent, HKEY regKey, CSalamanderRegistryAbstract* registry)
{
    CALL_STACK_MESSAGE1("CPluginInterface::SaveConfiguration(, ,)");

    DWORD v = CURRENT_CONFIG_VERSION;
    registry->SetValue(regKey, CONFIG_VERSION, REG_DWORD, &v, sizeof(DWORD));
    registry->SetValue(regKey, "DefaultSession", REG_SZ, SftpDefaultSession, -1);

    HKEY actKey;
    if (registry->CreateKey(regKey, CONFIG_KEY, actKey))
    {
        registry->SetValue(actKey, CONFIG_STR, REG_SZ, Str, -1);
        registry->CloseKey(actKey);
    }

    registry->SetValue(regKey, CONFIG_NUMBER, REG_DWORD, &Number, sizeof(DWORD));

    registry->SetValue(regKey, CONFIG_SAVEPOS, REG_DWORD, &CfgSavePosition, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_WNDPLACEMENT, REG_BINARY, &CfgWindowPlacement, sizeof(WINDOWPLACEMENT));
    registry->SetValue(regKey, CONFIG_SIZE2FIXEDWIDTH, REG_DWORD, &Size2FixedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_SIZE2WIDTH, REG_DWORD, &Size2Width, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_CREATEDFIXEDWIDTH, REG_DWORD, &CreatedFixedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_CREATEDWIDTH, REG_DWORD, &CreatedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_MODIFIEDFIXEDWIDTH, REG_DWORD, &ModifiedFixedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_MODIFIEDWIDTH, REG_DWORD, &ModifiedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_ACCESSEDFIXEDWIDTH, REG_DWORD, &AccessedFixedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_ACCESSEDWIDTH, REG_DWORD, &AccessedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_DFSTYPEFIXEDWIDTH, REG_DWORD, &DFSTypeFixedWidth, sizeof(DWORD));
    registry->SetValue(regKey, CONFIG_DFSTYPEWIDTH, REG_DWORD, &DFSTypeWidth, sizeof(DWORD));

    // ulož uložené servery (profily)
    HKEY serversKey;
    if (registry->CreateKey(regKey, "Servers", serversKey))
    {
        DWORD cnt = (DWORD)SftpProfileCount;
        registry->SetValue(serversKey, "Count", REG_DWORD, &cnt, sizeof(DWORD));
        for (int i = 0; i < SftpProfileCount; i++)
        {
            char idx[16];
            sprintf(idx, "%d", i);
            HKEY pk;
            if (registry->CreateKey(serversKey, idx, pk))
            {
                registry->SetValue(pk, "Name", REG_SZ, SftpProfiles[i].Name, -1);
                registry->SetValue(pk, "Host", REG_SZ, SftpProfiles[i].Host, -1);
                DWORD port = (DWORD)SftpProfiles[i].Port;
                registry->SetValue(pk, "Port", REG_DWORD, &port, sizeof(DWORD));
                registry->SetValue(pk, "User", REG_SZ, SftpProfiles[i].User, -1);
                registry->SetValue(pk, "Password", REG_SZ, SftpProfiles[i].Password, -1);
                registry->SetValue(pk, "KeyFile", REG_SZ, SftpProfiles[i].KeyFile, -1);
                registry->SetValue(pk, "Path", REG_SZ, SftpProfiles[i].Path, -1);
                DWORD comp = SftpProfiles[i].UseCompression ? 1 : 0;
                DWORD proto = (DWORD)SftpProfiles[i].Protocol;
                DWORD scpfb = SftpProfiles[i].ScpFallback ? 1 : 0;
                registry->SetValue(pk, "Compression", REG_DWORD, &comp, sizeof(DWORD));
                registry->SetValue(pk, "Protocol", REG_DWORD, &proto, sizeof(DWORD));
                registry->SetValue(pk, "ScpFallback", REG_DWORD, &scpfb, sizeof(DWORD));
                registry->CloseKey(pk);
            }
        }
        registry->CloseKey(serversKey);
    }
}

void WINAPI
CPluginInterface::Configuration(HWND parent)
{
    CALL_STACK_MESSAGE1("CPluginInterface::Configuration()");
    OnConfiguration(parent);
}

void WINAPI
CPluginInterface::Connect(HWND parent, CSalamanderConnectAbstract* salamander)
{
    CALL_STACK_MESSAGE1("CPluginInterface::Connect(,)");

#if !defined(ENABLE_DYNAMICMENUEXT)
    salamander->AddMenuItem(-1, "&Upravit soubor (server)", SALHOTKEY(VK_F4, 0), MENUCMD_EDITFILE, FALSE,
                            MENU_EVENT_TRUE, MENU_EVENT_THIS_PLUGIN_FS | MENU_EVENT_FILE_FOCUSED, MENU_SKILLLEVEL_ALL);
    salamander->AddMenuItem(-1, "&Spočítat velikost (server)", 0, MENUCMD_CALCSIZE, FALSE,
                            MENU_EVENT_TRUE, MENU_EVENT_THIS_PLUGIN_FS, MENU_SKILLLEVEL_ALL);
    salamander->AddMenuItem(-1, NULL, 0, 0, FALSE, 0, 0, MENU_SKILLLEVEL_ALL); // separator
    salamander->AddMenuItem(-1, "&Odpojit", 0, MENUCMD_DISCONNECT_ACTIVE, FALSE, MENU_EVENT_TRUE, MENU_EVENT_THIS_PLUGIN_FS, MENU_SKILLLEVEL_ALL);
#endif // !defined(ENABLE_DYNAMICMENUEXT)

    CGUIIconListAbstract* iconList = SalamanderGUI->CreateIconList();
    iconList->Create(16, 16, 1);
    HICON hIcon = (HICON)LoadImage(DLLInstance, MAKEINTRESOURCE(IDI_FS), IMAGE_ICON, 16, 16, SalamanderGeneral->GetIconLRFlags());
    iconList->ReplaceIcon(0, hIcon);
    DestroyIcon(hIcon);
    salamander->SetIconListForGUI(iconList); // Salamander handles destroying the icon list

    salamander->SetChangeDriveMenuItem("\tSFTP", 0);
    salamander->SetPluginIcon(0);
    salamander->SetPluginMenuAndToolbarIcon(0);
}

void WINAPI
CPluginInterface::ReleasePluginDataInterface(CPluginDataInterfaceAbstract* pluginData)
{
    if (pluginData != &ArcPluginDataInterface) // release an allocated FS data object if necessary
    {
        delete ((CPluginFSDataInterface*)pluginData);
    }
}

void WINAPI
CPluginInterface::ClearHistory(HWND parent)
{
    ViewerWindowQueue.BroadcastMessage(WM_USER_CLEARHISTORY, 0, 0);
    /*
  int i;
  for (i = 0; i < FIND_HISTORY_SIZE; i++)
  {
    if (FindHistory[i] != NULL)
    {
      free(FindHistory[i]);
      FindHistory[i] = NULL;
    }
  }
  */
}

void WINAPI
CPluginInterface::PasswordManagerEvent(HWND parent, int event)
{
    TRACE_I("PasswordManagerEvent(): event=" << event);
}

CPluginInterfaceForArchiverAbstract* WINAPI
CPluginInterface::GetInterfaceForArchiver()
{
    return &InterfaceForArchiver;
}

CPluginInterfaceForViewerAbstract* WINAPI
CPluginInterface::GetInterfaceForViewer()
{
    return &InterfaceForViewer;
}

CPluginInterfaceForMenuExtAbstract* WINAPI
CPluginInterface::GetInterfaceForMenuExt()
{
    return &InterfaceForMenuExt;
}

CPluginInterfaceForFSAbstract* WINAPI
CPluginInterface::GetInterfaceForFS()
{
    return &InterfaceForFS;
}

CPluginInterfaceForThumbLoaderAbstract* WINAPI
CPluginInterface::GetInterfaceForThumbLoader()
{
    return &InterfaceForThumbLoader;
}

void WINAPI
CPluginInterface::Event(int event, DWORD param)
{
    switch (event)
    {
    case PLUGINEVENT_COLORSCHANGED:
    {
        InitIconOverlays();

        // DFSImageList must be non-null, otherwise the entry point would report an error
        COLORREF bkColor = SalamanderGeneral->GetCurrentColor(SALCOL_ITEM_BK_NORMAL);
        if (ImageList_GetBkColor(DFSImageList) != bkColor)
            ImageList_SetBkColor(DFSImageList, bkColor);
        break;
    }

    case PLUGINEVENT_SETTINGCHANGE:
    {
        ViewerWindowQueue.BroadcastMessage(WM_USER_SETTINGCHANGE, 0, 0);
        break;
    }
    }
}
