/* wined3d's Vulkan pipeline cache file (patch 0003),
 * %LOCALAPPDATA%\wined3d-vk-cache\d3d9-pcache-test.exe.pipelines. The test
 * runs itself as a child ("draw": one draw, then exit, which saves the cache;
 * "idle": the device is created and released without drawing):
 *   unchanged: a run that creates no pipeline leaves the file alone (same
 *              size, same modification time). (A run that draws the same
 *              pipelines again still grows the file: CrossOver's MoltenVK
 *              writes loaded pipelines a second time.)
 *   truncated: a 16-byte file is replaced by a valid cache (>= 32 bytes)
 *   bad-uuid:  a cache whose header UUID was damaged is rewritten
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-pcache-test.exe d3d9-pcache-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>
#include "aio_test.h"

static const char vs_src[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";
static const char ps_src[] = "float4 main() : COLOR { return float4(0.25, 0.5, 0.75, 1.0); }";

static DWORD *compile(const char *src, size_t size, const char *profile)
{
    ID3DXBuffer *code, *errors = NULL;
    if (FAILED(D3DXCompileShader(src, size, NULL, NULL, "main", profile, 0, &code, &errors, NULL))) exit(3);
    return ID3DXBuffer_GetBufferPointer(code);
}

static int draw_once(BOOL draw)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DDevice9 *dev;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    struct { float x, y, z, w; } quad[4] = {{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,1}};

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) return 2;
    if (!draw)
    {
        IDirect3DDevice9_Release(dev);
        IDirect3D9_Release(d3d);
        return 0;
    }
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_3_0"), &ps);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DPixelShader9_Release(ps);
    IDirect3DDevice9_Release(dev);   /* the device saves the cache when it goes */
    IDirect3D9_Release(d3d);
    return 0;
}

static char cache_path[MAX_PATH];

static DWORD run_child(const char *mode)
{
    char cmd[MAX_PATH + 16];
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    DWORD code = 99;

    GetModuleFileNameA(NULL, cmd, MAX_PATH);
    strcat(cmd, " ");
    strcat(cmd, mode);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 98;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static BOOL file_info(DWORD *size, FILETIME *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(cache_path, GetFileExInfoStandard, &a)) return FALSE;
    *size = a.nFileSizeLow;
    if (mtime) *mtime = a.ftLastWriteTime;
    return TRUE;
}

static void write_bytes(DWORD offset, const void *data, DWORD len, BOOL truncate)
{
    HANDLE f = CreateFileA(cache_path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD done;
    SetFilePointer(f, offset, NULL, FILE_BEGIN);
    WriteFile(f, data, len, &done, NULL);
    if (truncate) SetEndOfFile(f);
    CloseHandle(f);
}

int main(int argc, char **argv)
{
    DWORD size1, size2, code;
    FILETIME t1, t2;
    char local[MAX_PATH];
    static const BYTE junk[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    if (argc > 1 && !strcmp(argv[1], "draw")) return draw_once(TRUE);
    if (argc > 1 && !strcmp(argv[1], "idle")) return draw_once(FALSE);

    GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    snprintf(cache_path, sizeof(cache_path), "%s\\wined3d-vk-cache\\d3d9-pcache-test.exe.pipelines", local);
    DeleteFileA(cache_path);

    /* unchanged */
    if ((code = run_child("draw"))) return report(0, "first draw failed (%lu)", code);
    if (!file_info(&size1, &t1)) return report(0, "no cache file after the first run: %s", cache_path);
    Sleep(1500);
    if ((code = run_child("idle"))) return report(0, "idle run failed (%lu)", code);
    file_info(&size2, &t2);
    if (size2 != size1 || CompareFileTime(&t1, &t2))
        return report(0, "unchanged: a run without new pipelines rewrote the cache (%lu -> %lu bytes)", size1, size2);

    /* truncated */
    write_bytes(0, junk, 16, TRUE);
    if ((code = run_child("draw"))) return report(0, "draw with a truncated cache failed (%lu)", code);
    if (!file_info(&size2, NULL) || size2 < 32)
        return report(0, "truncated: the cache is %lu bytes after the run (expected a valid cache)", size2);

    /* bad-uuid */
    write_bytes(16, junk, 16, FALSE);
    file_info(&size1, &t1);
    Sleep(1500);
    if ((code = run_child("draw"))) return report(0, "draw with a damaged cache failed (%lu)", code);
    file_info(&size2, &t2);
    if (!CompareFileTime(&t1, &t2))
        return report(0, "bad-uuid: a cache with a damaged UUID was kept as it was");

    return report(1, "unchanged, truncated, bad-uuid (%lu bytes)", size2);
}
