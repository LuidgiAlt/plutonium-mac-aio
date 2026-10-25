/* Draws a 16x16 quad whose vertices carry a D3DDECLTYPE_UBYTE4 attribute
 * (64, 128, 192, 255) and a D3DDECLTYPE_SHORT2 attribute (-32, 96). Direct3D 9
 * reads both as floats holding the integer values; the vertex shader outputs
 * them / 255 (SHORT2 as (x + 64) / 255).
 * Expected: UBYTE4 R 64 G 128 B 192, SHORT2 R 32 G 160.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-ubyte4-test.exe d3d9-ubyte4-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

static const char vs_src[] =
    "struct o { float4 p : POSITION; float4 c : COLOR0; };\n"
    "o main(float4 p : POSITION, float4 u : TEXCOORD0, float2 s : TEXCOORD1, uniform float mode)\n"
    "{ o r; r.p = p; r.c = mode > 0.5 ? float4((s + 64.0) / 255.0, 0.0, 1.0) : u / 255.0; return r; }";

static const char ps_src[] =
    "float4 main(float4 c : COLOR0) : COLOR { return c; }";

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
    int ok_ubyte4 = 0, ok_short2 = 0;
    DWORD px_ubyte4 = 0, px_short2 = 0;
    static const D3DVERTEXELEMENT9 elements[] =
    {
        {0, 0,  D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_UBYTE4, 0, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_SHORT2, 0, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    struct { float x, y, z, w; BYTE u[4]; short s[2]; } quad[4] =
    {
        {-1, 1,0,1, {64,128,192,255}, {-32,96}}, {1, 1,0,1, {64,128,192,255}, {-32,96}},
        {-1,-1,0,1, {64,128,192,255}, {-32,96}}, {1,-1,0,1, {64,128,192,255}, {-32,96}},
    };
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DDevice9 *dev;
    IDirect3DSurface9 *rt, *sys;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    D3DLOCKED_RECT lr;
    float mode[4] = {0};
    DWORD px;
    int pass;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile(vs_src, sizeof(vs_src) - 1, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_3_0"), &ps);
    IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl);
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    for (pass = 0; pass < 2; ++pass)
    {
        mode[0] = pass;
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetVertexShader(dev, vs);
        IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mode, 1);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetVertexDeclaration(dev, decl);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
        px = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 8 * 4);
        IDirect3DSurface9_UnlockRect(sys);
        if (!pass) ok_ubyte4 = rgb_near(px, 64, 128, 192), px_ubyte4 = px;
        else ok_short2 = channel_near(px, 16, 32) && channel_near(px, 8, 160), px_short2 = px;
    }
    return report(ok_ubyte4 && ok_short2, "UBYTE4 %06lx (expected ~40 80 c0), SHORT2 %06lx (expected ~20 a0 ..)",
                  px_ubyte4 & 0xffffff, px_short2 & 0xffffff);
}
