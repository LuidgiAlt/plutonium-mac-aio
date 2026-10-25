/* Switching only the vertex declaration between draws with one vertex
 * shader: UBYTE4 (64, 128, 192, 255) and FLOAT4 (the same values), drawn
 * alternately for 200 frames. Every frame must show ~40 80 c0 (patch 0009
 * re-selects the shader variant only when the integer inputs change).
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-declswitch-test.exe d3d9-declswitch-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

static const char vs_src[] =
    "void main(float4 p : POSITION, float4 v : TEXCOORD0, out float4 op : POSITION, out float4 oc : COLOR0)\n"
    "{ op = p; oc = v / 255.0; }";
static const char ps_src[] = "float4 main(float4 c : COLOR0) : COLOR { return c; }";

int main(void)
{
    static const D3DVERTEXELEMENT9 ubyte4[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 float4[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    struct { float x, y, z, w; BYTE b[4]; } qb[4] =
    {
        {-1,1,0,1, {64,128,192,255}}, {1,1,0,1, {64,128,192,255}}, {-1,-1,0,1, {64,128,192,255}}, {1,-1,0,1, {64,128,192,255}},
    };
    struct { float x, y, z, w; float f[4]; } qf[4] =
    {
        {-1,1,0,1, {64,128,192,255}}, {1,1,0,1, {64,128,192,255}}, {-1,-1,0,1, {64,128,192,255}}, {1,-1,0,1, {64,128,192,255}},
    };
    IDirect3DVertexDeclaration9 *db, *df;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD px;
    int frame;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexDeclaration(dev, ubyte4, &db);
    IDirect3DDevice9_CreateVertexDeclaration(dev, float4, &df);
    IDirect3DDevice9_CreateVertexShader(dev, compile_hlsl(vs_src, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile_hlsl(ps_src, "ps_3_0"), &ps);
    for (frame = 0; frame < 200; frame++)
    {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetVertexShader(dev, vs);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        if (frame & 1)
        {
            IDirect3DDevice9_SetVertexDeclaration(dev, df);
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, qf, sizeof(qf[0]));
        }
        else
        {
            IDirect3DDevice9_SetVertexDeclaration(dev, db);
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, qb, sizeof(qb[0]));
        }
        IDirect3DDevice9_EndScene(dev);
        px = read_pixel(8, 8) & 0xffffff;
        if (!rgb_near(px, 64, 128, 192))
            return report(0, "frame %d (%s): %06lx (expected ~40 80 c0)", frame, frame & 1 ? "FLOAT4" : "UBYTE4", px);
    }
    return report(1, "200 frames, UBYTE4 and FLOAT4 alternating");
}
