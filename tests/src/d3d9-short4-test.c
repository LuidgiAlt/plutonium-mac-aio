/* A D3DDECLTYPE_SHORT4 vertex attribute (64, 128, 191, 255) read by a vs_3_0
 * shader that outputs it / 255 (patch 0009 converts Direct3D 9 integer
 * attributes in the shader). Expected: R 64, G 128, B 191.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-short4-test.exe d3d9-short4-test.c -ld3d9 -ld3dx9 */
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
    static const D3DVERTEXELEMENT9 elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    struct { float x, y, z, w; short s[4]; } quad[4] =
    {
        {-1,1,0,1, {64,128,191,255}}, {1,1,0,1, {64,128,191,255}}, {-1,-1,0,1, {64,128,191,255}}, {1,-1,0,1, {64,128,191,255}},
    };
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD px;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl);
    IDirect3DDevice9_CreateVertexShader(dev, compile_hlsl(vs_src, "vs_3_0"), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, compile_hlsl(ps_src, "ps_3_0"), &ps);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    px = read_pixel(8, 8) & 0xffffff;
    return report(rgb_near(px, 64, 128, 191), "pixel %06lx (expected ~40 80 bf)", px);
}
