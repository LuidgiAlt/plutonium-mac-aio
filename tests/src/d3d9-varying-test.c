/* One vertex shader writes TEXCOORD0 = red and TEXCOORD1 = green. Pixel shader
 * A reads TEXCOORD0, pixel shader B reads TEXCOORD1. Draw the left half with A,
 * then switch only the pixel shader and draw the right half with B. The vertex
 * shader must be compiled again for B's inputs.
 * Expected: left ff0000, right 00ff00.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-varying-test.exe d3d9-varying-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

static const char vs_src[] =
    "struct o { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; };\n"
    "o main(float4 p : POSITION) { o r; r.p = p; r.t0 = float4(1, 0, 0, 1); r.t1 = float4(0, 1, 0, 1); return r; }";
static const char ps_a_src[] = "float4 main(float4 t0 : TEXCOORD0) : COLOR { return t0; }";
static const char ps_b_src[] = "float4 main(float4 t1 : TEXCOORD1) : COLOR { return t1; }";

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

static void half(IDirect3DDevice9 *dev, float x0, float x1)
{
    struct { float x, y, z, w; } v[4] = {{x0, 1, 0, 1}, {x1, 1, 0, 1}, {x0, -1, 0, 1}, {x1, -1, 0, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DPixelShader9 *ps_a, *ps_b;
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DSurface9 *rt, *sys;
    IDirect3DVertexShader9 *vs;
    IDirect3DDevice9 *dev;
    D3DLOCKED_RECT lr;
    DWORD left, right;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_a_src, sizeof(ps_a_src) - 1, "ps_3_0"), &ps_a);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_b_src, sizeof(ps_b_src) - 1, "ps_3_0"), &ps_b);
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps_a);
    half(dev, -1, 0);
    IDirect3DDevice9_SetPixelShader(dev, ps_b);
    half(dev, 0, 1);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    left = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 2 * 4) & 0xffffff;
    right = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 13 * 4) & 0xffffff;
    IDirect3DSurface9_UnlockRect(sys);
    return report((left & 0xffffff) == 0xff0000 && (right & 0xffffff) == 0x00ff00,
                  "left %06lx right %06lx (expected ff0000 00ff00)", left & 0xffffff, right & 0xffffff);
}
