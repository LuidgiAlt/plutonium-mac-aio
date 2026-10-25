/* 4096 draws into a 64x64 target: draw i covers pixel (i % 64, i / 64), with
 * its position in a vertex shader constant and its colour in a pixel shader
 * constant, both changed before every draw. Reads every pixel back and counts
 * mismatches. Checks that per-draw constant updates (constant buffer renames,
 * dynamic offsets) reach the right draw.
 * Expected: "mismatches 0".
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-constants-test.exe d3d9-constants-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

static const char vs_src[] =
    "float4 offset : register(c0);\n"
    "float4 main(float4 p : POSITION) : POSITION { return float4(p.xy * offset.zw + offset.xy, 0.0, 1.0); }";

static const char ps_src[] =
    "float4 colour : register(c0);\n"
    "float4 main() : COLOR { return colour; }";

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

static DWORD expected(unsigned int x, unsigned int y)
{
    return 0xff000000 | ((x * 4) << 16) | ((y * 4) << 8) | ((x ^ y) * 4);
}

int main(void)
{
    struct { float x, y, z, w; } quad[4] = {{0,0,0,1},{1,0,0,1},{0,-1,0,1},{1,-1,0,1}};
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DDevice9 *dev;
    IDirect3DSurface9 *rt, *sys;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    D3DLOCKED_RECT lr;
    unsigned int i, x, y, bad = 0;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_3_0"), &ps);
    IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    for (i = 0; i < 64 * 64; ++i)
    {
        DWORD c;
        float offset[4], colour[4];

        x = i % 64; y = i / 64;
        offset[0] = -1.0f + x * (2.0f / 64); offset[1] = 1.0f - y * (2.0f / 64);
        offset[2] = 2.0f / 64; offset[3] = 2.0f / 64;
        c = expected(x, y);
        colour[0] = ((c >> 16) & 0xff) / 255.0f; colour[1] = ((c >> 8) & 0xff) / 255.0f;
        colour[2] = (c & 0xff) / 255.0f; colour[3] = 1.0f;
        IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, offset, 1);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, colour, 1);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    for (y = 0; y < 64; ++y)
        for (x = 0; x < 64; ++x)
        {
            DWORD px = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
            if (px != expected(x, y))
            {
                if (bad < 5) printf("pixel %u,%u: %08lx, expected %08lx\n", x, y, px, expected(x, y));
                ++bad;
            }
        }
    IDirect3DSurface9_UnlockRect(sys);
    return report(bad == 0, "mismatches %u of 4096", bad);
}
