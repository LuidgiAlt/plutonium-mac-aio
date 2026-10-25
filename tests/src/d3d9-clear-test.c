/* Full clears between draws on the same render target and depth buffer, the
 * case where wined3d's Vulkan renderer restarted the render pass for every
 * clear. Sequence:
 *   clear colour black, depth 1.0; draw blue everywhere (z 0.5)
 *   clear colour green;            draw red on the left half (z 0.4)
*   clear depth+stencil 0.0;              draw yellow everywhere (z 0.25): must fail
*   clear depth+stencil 1.0;              draw magenta on the right half (z 0.9)
 * Expected: left red (ff0000), right magenta (ff00ff).
 * Then, in a new scene: draw blue everywhere, clear colour green (inside the
 * running render pass), draw red on the left half only.
 * Expected: right half green (00ff00): the clear really happened.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-clear-test.exe d3d9-clear-test.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"

struct vertex { float x, y, z, rhw; DWORD colour; };

static void quad(IDirect3DDevice9 *dev, float x0, float x1, float z, DWORD colour)
{
    struct vertex v[4] =
    {
        {x0, 0, z, 1, colour}, {x1, 0, z, 1, colour}, {x0, 16, z, 1, colour}, {x1, 16, z, 1, colour},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DSurface9 *rt, *ds, *sys;
    IDirect3DDevice9 *dev;
    D3DLOCKED_RECT lr;
    DWORD left, right;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateDepthStencilSurface(dev, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    quad(dev, 0, 16, 0.5f, 0xff0000ff);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    quad(dev, 0, 8, 0.4f, 0xffff0000);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.0f, 0);
    quad(dev, 0, 16, 0.25f, 0xffffff00);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    quad(dev, 8, 16, 0.9f, 0xffff00ff);
    IDirect3DDevice9_EndScene(dev);

    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    left = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 2 * 4) & 0xffffff;
    right = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 13 * 4) & 0xffffff;
    IDirect3DSurface9_UnlockRect(sys);
    {
        DWORD right2;
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
        IDirect3DDevice9_BeginScene(dev);
        quad(dev, 0, 16, 0.5f, 0xff0000ff);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
        quad(dev, 0, 8, 0.5f, 0xffff0000);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
        right2 = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 13 * 4) & 0xffffff;
        IDirect3DSurface9_UnlockRect(sys);
        return report((left & 0xffffff) == 0xff0000 && (right & 0xffffff) == 0xff00ff && right2 == 0x00ff00,
                      "left %06lx right %06lx (expected ff0000 ff00ff), cleared right %06lx (expected 00ff00)",
                      left & 0xffffff, right & 0xffffff, right2);
    }
}
