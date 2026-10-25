/* A managed volume texture rewritten every frame, as BO1 does, with enough
 * draws per frame that the command thread falls behind. Each frame: LockBox,
 * fill with the frame's colour, UnlockBox, 2000 small draws with it, one
 * full-target quad, Present. Reports the LockBox time and checks that the
 * last frame shows the last colour written.
 * Expected: "colour <last> (expected <last>)", and LockBox times far below a
 * frame (before patch 0016 each LockBox waited for the previous frame's
 * queued work).
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-managed-volume-test.exe d3d9-managed-volume-test.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"

#define SIZE 32
#define FRAMES 120
#define DRAWS 2000

struct vertex { float x, y, z, rhw; float u, v, w; };

static DWORD frame_colour(int frame)
{
    return 0xff000000 | ((frame * 37) & 0xff) << 16 | ((frame * 91) & 0xff) << 8 | ((frame * 13) & 0xff);
}

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    struct vertex quad[4] =
    {
        {0, 0, 0.5f, 1, 0, 0, 0.5f}, {64, 0, 0.5f, 1, 1, 0, 0.5f},
        {0, 64, 0.5f, 1, 0, 1, 0.5f}, {64, 64, 0.5f, 1, 1, 1, 0.5f},
    };
    struct vertex tri[3] = {{0, 0, 0.5f, 1, 0, 0, 0.5f}, {2, 0, 0.5f, 1, 1, 0, 0.5f}, {0, 2, 0.5f, 1, 0, 1, 0.5f}};
    IDirect3DVolumeTexture9 *texture;
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DSurface9 *rt, *sys;
    LARGE_INTEGER freq, t0, t1;
    double total_us = 0, max_us = 0;
    IDirect3DDevice9 *dev;
    D3DLOCKED_RECT lr;
    D3DLOCKED_BOX lb;
    int frame, i, x, y, z;
    DWORD colour;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    if (FAILED(IDirect3DDevice9_CreateVolumeTexture(dev, SIZE, SIZE, SIZE, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &texture, NULL)))
    { printf("CreateVolumeTexture failed\n"); return 1; }
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &rt);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)texture);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    QueryPerformanceFrequency(&freq);

    for (frame = 0; frame < FRAMES; ++frame)
    {
        colour = frame_colour(frame);
        QueryPerformanceCounter(&t0);
        if (FAILED(IDirect3DVolumeTexture9_LockBox(texture, 0, &lb, NULL, 0)))
        { printf("LockBox failed\n"); return 1; }
        QueryPerformanceCounter(&t1);
        for (z = 0; z < SIZE; ++z)
            for (y = 0; y < SIZE; ++y)
                for (x = 0; x < SIZE; ++x)
                    ((DWORD *)((BYTE *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = colour;
        IDirect3DVolumeTexture9_UnlockBox(texture, 0);
        if (frame >= 10) /* skip warm-up */
        {
            double us = (t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart;
            total_us += us;
            if (us > max_us) max_us = us;
        }

        IDirect3DDevice9_BeginScene(dev);
        for (i = 0; i < DRAWS; ++i)
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        IDirect3DDevice9_EndScene(dev);
        /* Read the last frame before Present: afterwards the back buffer is undefined. */
        if (frame == FRAMES - 1)
            IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    printf("LockBox average %.0f us, longest %.0f us\n", total_us / (FRAMES - 10), max_us);
    {
        unsigned int got = (unsigned int)(((DWORD *)((BYTE *)lr.pBits + 32 * lr.Pitch))[40] & 0xffffff);
        unsigned int want = (unsigned int)(frame_colour(FRAMES - 1) & 0xffffff);
        IDirect3DSurface9_UnlockRect(sys);
        return report(got == want, "colour %06x (expected %06x)", got, want);
    }
}
