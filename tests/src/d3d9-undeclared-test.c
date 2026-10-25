/* Input components a shader reads without declaring them (patch 0012):
 *   sm1: TEXCOORD0 is FLOAT2 (0.25, 0.5); vs_1_1 copies it to oT0, ps_1_1
 *        outputs it: the missing components are (z 0, w 1) -> 40 80 00
 *   sm3: vs_3_0 writes o1 = (0.25, 0.5, 0.75, 1.0); ps_3_0 declares only
 *        v0.xy and outputs v0.zwxy. Direct3D 9 leaves this undefined
 *        (hardware interpolates the whole register, DXVK reads 0); wined3d
 *        gives the defaults (z 0, w 1) instead of copies of x and y
 *        -> 00 ff 40
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-undeclared-test.exe d3d9-undeclared-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

static const char vs_src[] = "vs_1_1\ndcl_position v0\ndcl_texcoord v1\nmov oPos, v0\nmov oT0, v1\n";
static const char ps_src[] = "ps_1_1\ntexcoord t0\nmov r0, t0\n";
static const char vs3_src[] = "vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_texcoord0 o1\nmov o0, v0\nmov o1, c0\n";
static const char ps3_src[] = "ps_3_0\ndcl_texcoord0 v0.xy\nmov oC0, v0.zwxy\n";

int main(void)
{
    static const D3DVERTEXELEMENT9 elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    struct { float x, y, z, w, u, v; } quad[4] =
    {
        {-1,1,0,1, 0.25f,0.5f}, {1,1,0,1, 0.25f,0.5f}, {-1,-1,0,1, 0.25f,0.5f}, {1,-1,0,1, 0.25f,0.5f},
    };
    IDirect3DVertexDeclaration9 *decl;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD px, px3;
    static const float c0[4] = {0.25f, 0.5f, 0.75f, 1.0f};

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl);
    IDirect3DDevice9_CreateVertexShader(dev, assemble(vs_src), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, assemble(ps_src), &ps);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffffffff, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    px = read_pixel(8, 8) & 0xffffff;

    IDirect3DDevice9_CreateVertexShader(dev, assemble(vs3_src), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, assemble(ps3_src), &ps);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffffffff, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c0, 1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    px3 = read_pixel(8, 8) & 0xffffff;
    return report(rgb_near(px, 64, 128, 0) && rgb_near(px3, 0, 255, 64),
                  "sm1 %06lx (expected ~40 80 00), sm3 %06lx (expected ~00 ff 40)", px, px3);
}
