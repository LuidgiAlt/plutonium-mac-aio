/* Integer vertex inputs when the vertex shader changes on its own (patch
 * 0014 re-selects the variant when the integer inputs change). VS1 reads
 * TEXCOORD0 in v1; VS2 reads TEXCOORD1 in v1 and TEXCOORD0 in v2. Declaration
 * A has UBYTE4 TEXCOORD0 and FLOAT4 TEXCOORD1, B the other way round, both
 * holding (64, 128, 192, 255). Draws: VS1+A; then in one frame VS2+A (left)
 * and VS2+B (right). Every shader outputs its v1 / 255: ~40 80 c0.
 * VS2+B has the same integer inputs as VS1+A (v1), not VS2+A's (v2).
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-shaderswitch-test.exe d3d9-shaderswitch-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "aio_test.h"
#include "d3d9-common.h"

/* Assembly, so the input registers are exactly as described. */
static const char vs1_src[] =
    "vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_position o0\ndcl_color0 o1\n"
    "def c0, 0.00392156863, 0, 0, 0\nmov o0, v0\nmul o1, v1, c0.x\n";
static const char vs2_src[] =
    "vs_3_0\ndcl_position v0\ndcl_texcoord1 v1\ndcl_texcoord0 v2\ndcl_position o0\ndcl_color0 o1\ndcl_color1 o2\n"
    "def c0, 0.00392156863, 0, 0, 0\nmov o0, v0\nmul o1, v1, c0.x\nmov o2, v2\n";
static const char ps_src[] = "float4 main(float4 c : COLOR0) : COLOR { return c; }";

struct vertex { float x, y, z, w; BYTE ub[4]; float f[4]; };

int main(void)
{
    static const D3DVERTEXELEMENT9 a[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 b[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        {0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    struct vertex quad[4] =
    {
        {-1,1,0,1, {64,128,192,255}, {64,128,192,255}}, {1,1,0,1, {64,128,192,255}, {64,128,192,255}},
        {-1,-1,0,1, {64,128,192,255}, {64,128,192,255}}, {1,-1,0,1, {64,128,192,255}, {64,128,192,255}},
    };
    struct vertex left[4] =
    {
        {-1,1,0,1, {64,128,192,255}, {64,128,192,255}}, {0,1,0,1, {64,128,192,255}, {64,128,192,255}},
        {-1,-1,0,1, {64,128,192,255}, {64,128,192,255}}, {0,-1,0,1, {64,128,192,255}, {64,128,192,255}},
    };
    struct vertex right[4] =
    {
        {0,1,0,1, {64,128,192,255}, {64,128,192,255}}, {1,1,0,1, {64,128,192,255}, {64,128,192,255}},
        {0,-1,0,1, {64,128,192,255}, {64,128,192,255}}, {1,-1,0,1, {64,128,192,255}, {64,128,192,255}},
    };
    IDirect3DVertexDeclaration9 *da, *db;
    IDirect3DVertexShader9 *vs1, *vs2;
    IDirect3DPixelShader9 *ps;
    IDirect3DVertexBuffer9 *vb;
    DWORD px;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexDeclaration(dev, a, &da);
    IDirect3DDevice9_CreateVertexDeclaration(dev, b, &db);
    IDirect3DDevice9_CreateVertexShader(dev, assemble(vs1_src), &vs1);
    IDirect3DDevice9_CreateVertexShader(dev, assemble(vs2_src), &vs2);
    IDirect3DDevice9_CreatePixelShader(dev, compile_hlsl(ps_src, "ps_3_0"), &ps);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    /* One vertex buffer bound once (DrawPrimitiveUP rebinds the stream on
     * every draw, which would hide a stale state): full quad, left, right. */
    {
        void *p;
        IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(quad) * 3, 0, 0, D3DPOOL_MANAGED, &vb, NULL);
        IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0);
        memcpy(p, quad, sizeof(quad));
        memcpy((BYTE *)p + sizeof(quad), left, sizeof(left));
        memcpy((BYTE *)p + 2 * sizeof(quad), right, sizeof(right));
        IDirect3DVertexBuffer9_Unlock(vb);
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct vertex));
    }

    /* Frame 1: VS1+A. */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs1);
    IDirect3DDevice9_SetVertexDeclaration(dev, da);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    px = read_pixel(8, 8) & 0xffffff;
    if (!rgb_near(px, 64, 128, 192)) return report(0, "VS1+A: %06lx (expected ~40 80 c0)", px);

    /* Frame 2, no read-back in between (a blit would mark the states dirty
     * anyway): VS2+A on the left half (only the shader changes), then
     * VS2+B on the right half (only the declaration changes). */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs2);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 4, 2);
    IDirect3DDevice9_SetVertexDeclaration(dev, db);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 8, 2);
    IDirect3DDevice9_EndScene(dev);
    px = read_pixel(4, 8) & 0xffffff;
    if (!rgb_near(px, 64, 128, 192)) return report(0, "VS2+A: %06lx (expected ~40 80 c0)", px);
    px = read_pixel(12, 8) & 0xffffff;
    if (!rgb_near(px, 64, 128, 192)) return report(0, "VS2+B: %06lx (expected ~40 80 c0)", px);
    return report(1, "VS1+A, VS2+A, VS2+B");
}
