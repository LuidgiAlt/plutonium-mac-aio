/* vFace in an SM3 pixel shader (patch 0008): culling off, a back-facing
 * quad on the left half and a front-facing one on the right (DXVK as the
 * reference). The shader outputs R = vFace * 0.25 + 0.5: +1 for front faces
 * -> 191, -1 for back faces -> 64 (NaN or 0 would show as 0 or 128).
 * Expected: (4, 8) R 64, (12, 8) R 191.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-vface-test.exe d3d9-vface-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

static const char vs_src[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";
static const char ps_src[] =
    "float4 main(float face : VFACE) : COLOR { return float4(face * 0.25 + 0.5, 0, 0, 1); }";

int main(void)
{
    /* Left half wound one way, right half the other: the left one is the
     * back face (as DXVK renders it). */
    struct { float x, y, z, w; } tris[12] =
    {
        {-1,-1,0,1}, {0,-1,0,1}, {-1,1,0,1},   {-1,1,0,1}, {0,-1,0,1}, {0,1,0,1},
        {0,-1,0,1}, {0,1,0,1}, {1,-1,0,1},     {1,-1,0,1}, {0,1,0,1}, {1,1,0,1},
    };
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD left, right;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile_hlsl(vs_src, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile_hlsl(ps_src, "ps_3_0"), &ps);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff0000ff, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 4, tris, sizeof(tris[0]));
    IDirect3DDevice9_EndScene(dev);
    left = read_pixel(4, 8) & 0xffffff;
    right = read_pixel(12, 8) & 0xffffff;
    return report(channel_near(left, 16, 64) && channel_near(right, 16, 191),
                  "back R %lu (expected 64), front R %lu (expected 191)", (left >> 16) & 0xff, (right >> 16) & 0xff);
}
