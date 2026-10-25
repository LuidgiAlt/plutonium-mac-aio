/* Draws a 16x16 quad with vs_3_0/ps_3_0 (HLSL, compiled at run time); the pixel shader outputs vPos / 16.
 * Reads pixel (5, 3). Direct3D 9's vPos is the integer pixel position, so
 * expected R ~ 80 (5/16), G ~ 48 (3/16), B 255. 88/56 means the 0.5 pixel
 * centre offset leaked through; 0/0 means vPos is not read at all.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-vpos-test.exe d3d9-vpos-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

static const char vs_src[] =
    "float4 main(float4 p : POSITION) : POSITION { return p; }";

static const char ps_src[] =
    "float4 main(float2 p : VPOS) : COLOR { return float4(p / 16.0, 1.0, 1.0); }";

static DWORD *compile(const char *src, size_t size, const char *profile)
{
    ID3DXBuffer *code, *errors = NULL;
    if (FAILED(D3DXCompileShader(src, size, NULL, NULL, "main", profile, 0, &code, &errors, NULL)))
    {
        printf("assemble failed: %s\n", errors ? (char *)ID3DXBuffer_GetBufferPointer(errors) : "?");
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
    IDirect3DSurface9 *rt, *sys;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    D3DLOCKED_RECT lr;
    struct { float x, y, z, w; } quad[4] = {{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,1}};
    DWORD px;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_3_0"), &ps);
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    px = *(DWORD *)((BYTE *)lr.pBits + 3 * lr.Pitch + 5 * 4);
    IDirect3DSurface9_UnlockRect(sys);
    return report(rgb_near(px, 80, 48, 255), "pixel R %lu G %lu B %lu (expected ~80, ~48, 255)", (px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff);
}
