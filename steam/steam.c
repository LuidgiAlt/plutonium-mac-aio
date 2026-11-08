/* steam.exe for GAME=BO2_SP: stands in for Steam's Windows client inside the
 * prefix while the macOS Steam app does the real work. A game wrapped in
 * Steam's DRM looks for a running Steam: the ActiveProcess keys naming this
 * process and the steamclient.dll to load, a couple of named objects, and a
 * hidden "Steam" window. Valve's steamclient.dll is redirected to
 * lsteamclient.dll by the engine (patch 0024), which talks to macOS Steam.
 *
 *   steam.exe <game.exe> [args]   set up, start the game, wait for it
 *   steam.exe steam://...         hand the link to macOS Steam
 *
 * SteamAppId names the game. Build: x86_64-w64-mingw32-gcc -municode -mwindows
 * -O2 -o steam.exe steam.c -ladvapi32 -lshell32 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static const WCHAR steam_dir[] = L"C:\\Program Files (x86)\\Steam";

static void set_string( HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *value )
{
    RegSetKeyValueW( root, key, name, REG_SZ, value, (DWORD)((wcslen( value ) + 1) * sizeof(WCHAR)) );
}

static void set_dword( HKEY root, const WCHAR *key, const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( root, key, name, REG_DWORD, &value, sizeof(value) );
}

static void setup_registry( const WCHAR *appid, DWORD running )
{
    static const WCHAR active[] = L"Software\\Valve\\Steam\\ActiveProcess";
    static const WCHAR steam[] = L"Software\\Valve\\Steam";
    WCHAR path[MAX_PATH], app_key[64];

    set_dword( HKEY_CURRENT_USER, active, L"pid", running ? GetCurrentProcessId() : 0 );
    swprintf( path, MAX_PATH, L"%ls\\steamclient.dll", steam_dir );
    set_string( HKEY_CURRENT_USER, active, L"SteamClientDll", path );
    swprintf( path, MAX_PATH, L"%ls\\steamclient64.dll", steam_dir );
    set_string( HKEY_CURRENT_USER, active, L"SteamClientDll64", path );
    set_string( HKEY_CURRENT_USER, active, L"SteamPath", steam_dir );
    set_string( HKEY_CURRENT_USER, active, L"Universe", L"Public" );
    set_string( HKEY_CURRENT_USER, steam, L"SteamPath", steam_dir );
    swprintf( path, MAX_PATH, L"%ls\\steam.exe", steam_dir );
    set_string( HKEY_CURRENT_USER, steam, L"SteamExe", path );
    if (!getenv( "STEAM_LANGUAGE" )) set_string( HKEY_CURRENT_USER, steam, L"language", L"english" );
    set_string( HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", steam_dir );
    set_string( HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Valve\\Steam", L"InstallPath", steam_dir );
    if (appid && *appid)
    {
        swprintf( app_key, ARRAYSIZE(app_key), L"%ls\\Apps\\%ls", steam, appid );
        set_dword( HKEY_CURRENT_USER, app_key, L"Installed", 1 );
        set_dword( HKEY_CURRENT_USER, app_key, L"Running", running );
        set_dword( HKEY_CURRENT_USER, app_key, L"Updating", 0 );
    }
}

/* Some games look for Steam's main window. */
static DWORD WINAPI steam_window( void *arg )
{
    WNDCLASSEXW cls = { sizeof(cls) };
    MSG msg;

    (void)arg;
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"vguiPopupWindow";
    RegisterClassExW( &cls );
    CreateWindowW( cls.lpszClassName, L"Steam", WS_POPUP, 40, 40, 400, 300, NULL, NULL, NULL, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

/* Older DRM wrappers ask Steam to acknowledge their start: they post on these
 * semaphores and wait for an event whose name starts with
 * STEAM_START_ACK_EVENT, found by listing the session's named objects. */
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } NT_STRING;
typedef struct { NT_STRING ObjectName, ObjectTypeName; } NT_DIRECTORY_INFO;
typedef struct { ULONG Length; HANDLE RootDirectory; NT_STRING *ObjectName; ULONG Attributes;
                 void *SecurityDescriptor, *SecurityQualityOfService; } NT_OBJECT_ATTRIBUTES;
typedef LONG (WINAPI *open_dir_fn)( HANDLE *, ACCESS_MASK, NT_OBJECT_ATTRIBUTES * );
typedef LONG (WINAPI *query_dir_fn)( HANDLE, void *, ULONG, BOOLEAN, BOOLEAN, ULONG *, ULONG * );

static HANDLE find_ack_event( void )
{
    static const WCHAR prefix[] = L"STEAM_START_ACK_EVENT";
    static WCHAR dir_name[] = L"\\BaseNamedObjects\\Session\\1";
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    open_dir_fn open_dir = (open_dir_fn)(void *)GetProcAddress( ntdll, "NtOpenDirectoryObject" );
    query_dir_fn query_dir = (query_dir_fn)(void *)GetProcAddress( ntdll, "NtQueryDirectoryObject" );
    NT_STRING str = { sizeof(dir_name) - sizeof(WCHAR), sizeof(dir_name), dir_name };
    NT_OBJECT_ATTRIBUTES attr = { sizeof(attr), NULL, &str, 0, NULL, NULL };
    union { NT_DIRECTORY_INFO info; char bytes[1024]; } buf;
    HANDLE dir, ret = NULL;
    ULONG context = 0, size;
    BOOLEAN first = TRUE;

    if (!open_dir || !query_dir || open_dir( &dir, 1 /* DIRECTORY_QUERY */, &attr )) return NULL;
    while (!ret && !query_dir( dir, &buf, sizeof(buf), TRUE, first, &context, &size ))
    {
        WCHAR name[MAX_PATH];
        int len = buf.info.ObjectName.Length / sizeof(WCHAR);

        first = FALSE;
        if (len >= MAX_PATH) continue;
        memcpy( name, buf.info.ObjectName.Buffer, len * sizeof(WCHAR) );
        name[len] = 0;
        if (!wcsncmp( name, prefix, ARRAYSIZE(prefix) - 1 )) ret = OpenEventW( EVENT_MODIFY_STATE, FALSE, name );
    }
    CloseHandle( dir );
    return ret;
}

static DWORD WINAPI drm_handshake( void *arg )
{
    HANDLE consume = CreateSemaphoreW( NULL, 0, 512, L"STEAM_DIPC_CONSUME" );
    HANDLE produce = CreateSemaphoreW( NULL, 1, 512, L"SREAM_DIPC_PRODUCE" );  /* sic, Valve's name */
    HANDLE ack = NULL;

    (void)arg;
    if (!consume || !produce) return 1;
    while (WaitForSingleObject( consume, INFINITE ) == WAIT_OBJECT_0)
    {
        if (!ack) ack = find_ack_event();
        if (ack) SetEvent( ack );
        ReleaseSemaphore( produce, 1, NULL );
    }
    return 0;
}

/* The command line after argv[0], untouched. */
static WCHAR *rest_of_command_line( void )
{
    WCHAR *p = GetCommandLineW();
    BOOL quoted = FALSE;

    for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++)
        if (*p == '"') quoted = !quoted;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int wmain( int argc, WCHAR *argv[] )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR appid[32] = L"", *cmdline;
    DWORD code = 1;

    if (argc < 2)
    {
        fwprintf( stderr, L"usage: steam.exe <game.exe> [args] | steam://...\n" );
        return 2;
    }
    if (!_wcsnicmp( argv[1], L"steam://", 8 ))
    {
        /* winebrowser opens it with macOS, which hands it to the Steam app. */
        return (INT_PTR)ShellExecuteW( NULL, L"open", argv[1], NULL, NULL, SW_SHOWNORMAL ) > 32 ? 0 : 1;
    }

    GetEnvironmentVariableW( L"SteamAppId", appid, ARRAYSIZE(appid) );
    if (!*appid) GetEnvironmentVariableW( L"SteamGameId", appid, ARRAYSIZE(appid) );
    CreateEventW( NULL, FALSE, FALSE, L"Steam3Master_SharedMemLock" );
    CreateEventW( NULL, FALSE, FALSE, L"Global\\Valve_SteamIPC_Class" );
    setup_registry( appid, 1 );
    CreateThread( NULL, 0, steam_window, NULL, 0, NULL );
    CreateThread( NULL, 0, drm_handshake, NULL, 0, NULL );

    cmdline = _wcsdup( rest_of_command_line() );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        fwprintf( stderr, L"steam.exe: cannot start %ls (error %lu)\n", argv[1], GetLastError() );
        setup_registry( appid, 0 );
        return 1;
    }
    CloseHandle( pi.hThread );
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    setup_registry( appid, 0 );
    return (int)code;
}
