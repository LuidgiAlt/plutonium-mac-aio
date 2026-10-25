/* Hardware shadow-map sampling (dxvk 0002): a D24S8 depth texture holding 0.5
 * is sampled with tex2Dproj and reference depth 0.25 on the left half and
 * 0.75 on the right. Direct3D 9 drivers return the comparison (reference <=
 * stored depth): left 1.0 (lit), right 0.0 (shadowed). Without it, the depth
 * value itself (0.5, ~128) comes back on both halves.
 * Expected: pixel (4, 8) = 255, pixel (12, 8) = 0.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-shadow-test.exe d3d9-shadow-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>
#include "aio_test.h"

static const char ps_src[] =
    "sampler2D s0;\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float ref = uv.x < 0.5 ? 0.25 : 0.75;\n"
    "    float r = tex2Dproj(s0, float4(uv, ref, 1.0)).r;\n"
    "    return float4(r, r, r, 1.0);\n"
    "}\n";

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
    IDirect3DTexture9 *depth;
    IDirect3DSurface9 *depth_surface, *rt, *sys;
    IDirect3DPixelShader9 *ps;
    D3DLOCKED_RECT lr;
    DWORD left, right;
    struct { float x, y, z, rhw; DWORD colour; } depth_quad[4] =
    {
        {0, 0, 0.5f, 1, 0xffffffff}, {16, 0, 0.5f, 1, 0xffffffff}, {0, 16, 0.5f, 1, 0xffffffff}, {16, 16, 0.5f, 1, 0xffffffff},
    };
    struct { float x, y, z, rhw, u, v; } quad[4] =
    {
        {-0.5f, -0.5f, 0, 1, 0, 0}, {15.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, 15.5f, 0, 1, 0, 1}, {15.5f, 15.5f, 0, 1, 1, 1},
    };

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return report(0, "CreateDevice failed");
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 16, 16, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &depth, NULL)))
        return skip("no D24S8 depth textures");
    IDirect3DTexture9_GetSurfaceLevel(depth, 0, &depth_surface);
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_CreatePixelShader(dev, compile(ps_src, sizeof(ps_src) - 1, "ps_2_0"), &ps);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);

    /* Depth pass: 0.5 everywhere. */
    IDirect3DDevice9_SetDepthStencilSurface(dev, depth_surface);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, depth_quad, sizeof(depth_quad[0]));
    IDirect3DDevice9_EndScene(dev);

    /* Shadow pass: sample the depth texture with a reference per half. */
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)depth);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff0000ff, 1.0f, 0);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);

    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    left = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 4 * 4) & 0xffffff;
    right = *(DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch + 12 * 4) & 0xffffff;
    IDirect3DSurface9_UnlockRect(sys);
    return report(rgb_near(left, 255, 255, 255) && rgb_near(right, 0, 0, 0),
                  "left %06lx (expected ffffff, lit), right %06lx (expected 000000, shadowed)", left, right);
}
