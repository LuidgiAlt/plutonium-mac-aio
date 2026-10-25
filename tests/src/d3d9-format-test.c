/* Creates A8L8 and L8 textures, draws each with the fixed-function pipeline and
 * reads back one pixel. Expected: A8L8 (L 0x40, A 0x80) -> R G B 64, A 128;
 * L8 (0x40) -> R G B 64, A 255.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-format-test.exe d3d9-format-test.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"

static IDirect3DDevice9 *dev;
static IDirect3DSurface9 *rt, *sys;

static int test(const char *name, D3DFORMAT fmt, WORD texel, int bytes, unsigned int alpha)
{
    struct { float x, y, z, rhw, u, v; } quad[4] = {{-1,-1,0,1,0,0},{17,-1,0,1,1,0},{-1,17,0,1,0,1},{17,17,0,1,1,1}};
    IDirect3DTexture9 *tex;
    D3DLOCKED_RECT lr;
    DWORD px;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, fmt, D3DPOOL_MANAGED, &tex, NULL);
    int y, x;

    if (FAILED(hr)) { printf("%-5s CreateTexture FAILED hr %#lx\n", name, hr); return 0; }
    IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
    {
        if (bytes == 2) ((WORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = texel;
        else ((BYTE *)lr.pBits + y * lr.Pitch)[x] = (BYTE)texel;
    }
    IDirect3DTexture9_UnlockRect(tex, 0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    px = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 8 * 4);
    IDirect3DSurface9_UnlockRect(sys);
    printf("%-5s R %3lu G %3lu B %3lu A %3lu\n", name, (px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff, px >> 24);
    IDirect3DTexture9_Release(tex);
    return rgb_near(px, 64, 64, 64) && channel_near(px, 24, alpha);
}

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    {
        int a8l8 = test("A8L8", D3DFMT_A8L8, 0x8040, 2, 128);
        int l8 = test("L8", D3DFMT_L8, 0x40, 1, 255);
        return report(a8l8 && l8, "A8L8 %s, L8 %s (expected RGB 64, A 128 / 255)", a8l8 ? "ok" : "wrong", l8 ? "ok" : "wrong");
    }
}
