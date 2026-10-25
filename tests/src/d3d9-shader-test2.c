/* Draws one pixel with a ps_2_0 shader using nrm and lrp, reads it back.
 * Expected: R ~ 121 (lrp(0.25, 1.0, 0.3) = 0.475), G ~ 153 (nrm(3, 4, 0).x = 0.6), B 255.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-shader-test2.exe d3d9-shader-test2.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

static const char ps_src[] =
    "ps_2_0\n"
    "def c0, 0.5, 2.0, 1.0, 1.0\n"
    "def c1, 0.3, 0.4, 0.1, 0.0\n"
    "def c2, 3.0, 4.0, 0.0, 0.0\n"
    "def c3, 0.25, 0.0, 0.0, 0.0\n"
    "nrm r1, c2\n"
    "lrp r0.x, c3.x, c0.z, c1.x\n"
    "mov r0.y, r1.x\n"
    "mov r0.zw, c0.z\n"
    "mov oC0, r0\n";

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DDevice9 *dev;
    IDirect3DSurface9 *rt, *sys;
    IDirect3DPixelShader9 *ps;
    ID3DXBuffer *code, *errors = NULL;
    D3DLOCKED_RECT lr;
    struct { float x, y, z, rhw; } quad[4] = {{-1,-1,0,1},{17,-1,0,1},{-1,17,0,1},{17,17,0,1}};
    DWORD px;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    if (FAILED(D3DXAssembleShader(ps_src, sizeof(ps_src) - 1, NULL, NULL, 0, &code, &errors)))
    { printf("assemble failed: %s\n", errors ? (char *)ID3DXBuffer_GetBufferPointer(errors) : "?"); return 1; }
    IDirect3DDevice9_CreatePixelShader(dev, ID3DXBuffer_GetBufferPointer(code), &ps);
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    px = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 8 * 4);
    IDirect3DSurface9_UnlockRect(sys);
    return report(rgb_near(px, 121, 153, 255), "pixel R %lu G %lu B %lu (expected ~121, ~153, 255)", (px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff);
}
