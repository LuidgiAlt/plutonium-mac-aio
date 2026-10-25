/* DXVK's background pipeline compiler (dxvk 0003) with every compile forced to
 * fail (DXVK_ASYNC_FAIL_TEST=1): the same draw state for 100 frames must be
 * compiled once, not once per frame. The program only checks that every frame
 * presents; tests/cases.txt counts "async compile failed" in DXVK's log.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-async-fail-test.exe d3d9-async-fail-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>
#include "aio_test.h"

static const char vs_src[] =
    "float4 main(float4 p : POSITION) : POSITION { return p; }";

static const char ps_src[] =
    "float4 main() : COLOR { return float4(1.0, 0.5, 0.25, 1.0); }";

static DWORD *compile(const char *src, size_t size, const char *profile)
{
    ID3DXBuffer *code, *errors = NULL;
    if (FAILED(D3DXCompileShader(src, size, NULL, NULL, "main", profile, 0, &code, &errors, NULL)))
    {
        printf("compile failed: %s\n", errors ? (char *)ID3DXBuffer_GetBufferPointer(errors) : "?");
        exit(1);
    }
    return ID3DXBuffer_GetBufferPointer(code);
}

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DDevice9 *dev;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    struct { float x, y, z, w; } quad[4] = {{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,1}};
    int frame, presented = 0;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_3_0"), &ps);
    for (frame = 0; frame < 100; frame++)
    {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetVertexShader(dev, vs);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        IDirect3DDevice9_EndScene(dev);
        if (SUCCEEDED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) presented++;
    }
    return report(presented == 100, "%d of 100 frames presented", presented);
}
