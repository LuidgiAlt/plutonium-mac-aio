/* A burst of new pipelines in one frame with DXVK's background compiler
 * (dxvk 0003, DXVK_ASYNC=1): 40 pixel shaders never drawn before, one draw
 * each, timed from the first draw until an event query issued after Present
 * signals (DXVK records draws on its own thread). Waiting up to 3 ms
 * for every new pipeline would add up to 120 ms to that frame; the waits
 * share a budget, so the frame must take less than 50 ms.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-async-burst-test.exe d3d9-async-burst-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

#define SHADERS 40

static const char vs_src[] = "float4 main(float4 p : POSITION) : POSITION { return p; }";

int main(void)
{
    struct { float x, y, z, w; } quad[4] = {{-1,1,0,1},{1,1,0,1},{-1,-1,0,1},{1,-1,0,1}};
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps[SHADERS];
    IDirect3DQuery9 *query;
    LARGE_INTEGER f, t0, t1;
    char src[128];
    double ms;
    int i;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_CreateVertexShader(dev, compile_hlsl(vs_src, "vs_3_0"), &vs);
    for (i = 0; i < SHADERS; i++)
    {
        sprintf(src, "float4 main() : COLOR { return float4(%d.0 / 255.0, 0.25, 0.5, 1.0); }", i + 1);
        IDirect3DDevice9_CreatePixelShader(dev, compile_hlsl(src, "ps_3_0"), &ps[i]);
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZW);
    for (i = 0; i < SHADERS; i++)
    {
        IDirect3DDevice9_SetPixelShader(dev, ps[i]);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &query);
    IDirect3DQuery9_Issue(query, D3DISSUE_END);
    while (IDirect3DQuery9_GetData(query, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
    QueryPerformanceCounter(&t1);
    ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    return report(ms < 50.0, "frame with %d new pipelines took %.1f ms (max 50)", SHADERS, ms);
}
