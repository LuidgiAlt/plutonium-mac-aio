/* A texture matrix set while a vertex shader is bound must still apply to the
 * next fixed-function draw (patch 0019 delays the upload until then).
 * Texture: 2x1, left texel red, right texel green, point filtering.
 * Sequence: bind a vertex shader, set the texture 0 matrix u' = u / 2 + 0.5
 * with TEXTURETRANSFORMFLAGS COUNT2, draw; then unbind the vertex shader and
 * draw a quad with u = 0.25 everywhere through the fixed-function pipeline.
 * With the matrix u' = 0.625 (green); without it u = 0.25 (red).
 * Then the same with the identity matrix set while the shader is bound: red.
 * Expected: "with matrix 00ff00 (expected 00ff00), identity ff0000 (expected ff0000)".
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-texmatrix-test.exe d3d9-texmatrix-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include "aio_test.h"

struct vertex { float x, y, z; float u, v; };

static DWORD draw_and_read(IDirect3DDevice9 *dev, IDirect3DVertexShader9 *vs, IDirect3DSurface9 *rt,
        IDirect3DSurface9 *sys, const D3DMATRIX *matrix)
{
    struct vertex quad[4] =
    {
        {-1, 1, 0.5f, 0.25f, 0.5f}, {1, 1, 0.5f, 0.25f, 0.5f},
        {-1, -1, 0.5f, 0.25f, 0.5f}, {1, -1, 0.5f, 0.25f, 0.5f},
    };
    D3DLOCKED_RECT lr;
    DWORD colour;

    IDirect3DDevice9_BeginScene(dev);
    /* Matrix changed while a vertex shader is bound. */
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetTransform(dev, D3DTS_TEXTURE0, matrix);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    /* Fixed-function draw, same vertex declaration. */
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);

    IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    colour = ((DWORD *)((BYTE *)lr.pBits + 8 * lr.Pitch))[8] & 0xffffff;
    IDirect3DSurface9_UnlockRect(sys);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    return colour;
}

int main(void)
{
    static const char vs_src[] = "vs_1_1\ndcl_position v0\nmov oPos, v0\n";
    static const D3DVERTEXELEMENT9 elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    D3DMATRIX half = {{{0.5f, 0, 0, 0, 0, 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1}}};
    IDirect3DVertexDeclaration9 *decl;
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DVertexShader9 *vs;
    IDirect3DSurface9 *rt, *sys;
    IDirect3DTexture9 *texture;
    IDirect3DDevice9 *dev;
    ID3DXBuffer *code;
    D3DLOCKED_RECT lr;
    DWORD with, without;

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return report(0, "CreateDevice failed");
    if (FAILED(D3DXAssembleShader(vs_src, sizeof(vs_src) - 1, NULL, NULL, 0, &code, NULL))
            || FAILED(IDirect3DDevice9_CreateVertexShader(dev, ID3DXBuffer_GetBufferPointer(code), &vs)))
    { printf("vertex shader failed\n"); return 1; }
    IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl);
    IDirect3DDevice9_CreateTexture(dev, 2, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    IDirect3DTexture9_LockRect(texture, 0, &lr, NULL, 0);
    ((DWORD *)lr.pBits)[0] = 0xffff0000;
    ((DWORD *)lr.pBits)[1] = 0xff00ff00;
    IDirect3DTexture9_UnlockRect(texture, 0);
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &rt);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);

    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)texture);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);

    /* A fixed-function draw first, so the identity matrix is uploaded. */
    draw_and_read(dev, NULL, rt, sys, &identity);
    with = draw_and_read(dev, vs, rt, sys, &half);
    without = draw_and_read(dev, vs, rt, sys, &identity);
    return report((with & 0xffffff) == 0x00ff00 && (without & 0xffffff) == 0xff0000,
                  "with matrix %06x (expected 00ff00), identity %06x (expected ff0000)",
                  (unsigned int)(with & 0xffffff), (unsigned int)(without & 0xffffff));
}
