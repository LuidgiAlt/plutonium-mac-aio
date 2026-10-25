/* Edge cases of the d3dbc pow and nrm instructions lowered by vkd3d-shader
 * (patch 0006), in assembly (Wine's HLSL compiler emits its own log/mul/exp
 * for pow), c0 = (0, 0, 2, 0), c1 = 0; each a full-target draw read at (8, 8):
 *   pow(0, 0) = 1                         -> R 255
 *   normalize(0) * 0.5 + 0.5, no NaN       -> R G B 128 (wined3d's GLSL gives 0.5)
 *   pow(-2, 2) through a negate modifier  -> saturate(4 / 4) = 1 -> R 255
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-math-edge-test.exe d3d9-math-edge-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

static const char vs_src[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";
static const char *ps_src[] =
{
    "ps_3_0\ndef c3, 0, 0, 0, 1\npow r0.x, c0.x, c0.y\nmov r1, c3\nmov r1.x, r0.x\nmov oC0, r1\n",
    "ps_3_0\ndef c3, 0.5, 0.5, 0.5, 1\nnrm r0.xyz, c1\nmad r1.xyz, r0, c3, c3\nmov r1.w, c3.w\nmov oC0, r1\n",
    "ps_3_0\ndef c3, 0.25, 0, 0, 1\ndef c4, 2, 0, 0, 0\npow r0.x, -c0.z, c4.x\nmov r1, c3\nmul_sat r1.x, r0.x, c3.x\nmov oC0, r1\n",
};

int main(void)
{
    static const float c0[4] = {0.0f, 0.0f, 2.0f, 0.0f}, c1[4] = {0};
    struct { float x, y, z, w; } quad[4] = {{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,1}};
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    DWORD px[3];
    int i;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile_hlsl(vs_src, "vs_3_0"), &vs);
    for (i = 0; i < 3; i++)
    {
        IDirect3DDevice9_CreatePixelShader(dev, assemble(ps_src[i]), &ps);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff0000ff, 0, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetVertexShader(dev, vs);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c1, 1);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        IDirect3DDevice9_EndScene(dev);
        px[i] = read_pixel(8, 8) & 0xffffff;
        IDirect3DPixelShader9_Release(ps);
    }
    return report(rgb_near(px[0], 255, 0, 0) && rgb_near(px[1], 128, 128, 128) && rgb_near(px[2], 255, 0, 0),
                  "pow(0,0) %06lx (expected ff0000), normalize(0) %06lx (expected 808080), pow(-2,2) %06lx (expected ff0000)",
                  px[0], px[1], px[2]);
}
