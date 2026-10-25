/* Software vertex processing (ProcessVertices) through DXVK. Its emulation
 * needs a geometry shader, which MoltenVK 1.4.2 does not have: DXVK must
 * refuse cleanly (dxvk 0001). One vertex at the origin, identity transforms,
 * 16x16 viewport: a correct result is screen position (8, 8). PASS when the
 * output is correct, or untouched (refused), or CreateDevice fails; FAIL on
 * wrong data. tests/cases.txt also checks that no pipeline failed to compile.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-swvp-test.exe d3d9-swvp-test.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "aio_test.h"

int main(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3DDevice9 *dev;
    IDirect3DVertexBuffer9 *src, *dst;
    D3DMATRIX identity = {{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}};
    D3DVIEWPORT9 vp = {0, 0, 16, 16, 0.0f, 1.0f};
    float *v, out[4];
    static const float origin[3] = {0, 0, 0};
    static const D3DVERTEXELEMENT9 out_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *out_decl;
    DWORD sentinel;
    HRESULT hr;

    memset(&sentinel, 0xcd, sizeof(sentinel));
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)))
        return report(1, "CreateDevice with software vertex processing refused");

    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(origin), D3DUSAGE_SOFTWAREPROCESSING, D3DFVF_XYZ, D3DPOOL_SYSTEMMEM, &src, NULL);
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(out), D3DUSAGE_SOFTWAREPROCESSING, D3DFVF_XYZRHW, D3DPOOL_SYSTEMMEM, &dst, NULL);
    IDirect3DVertexBuffer9_Lock(src, 0, 0, (void **)&v, 0);
    memcpy(v, origin, sizeof(origin));
    IDirect3DVertexBuffer9_Unlock(src);
    IDirect3DVertexBuffer9_Lock(dst, 0, 0, (void **)&v, 0);
    memset(v, 0xcd, sizeof(out));
    IDirect3DVertexBuffer9_Unlock(dst);

    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &identity);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &identity);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &identity);
    IDirect3DDevice9_SetViewport(dev, &vp);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetStreamSource(dev, 0, src, 0, sizeof(origin));
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ);
    /* DXVK requires the output declaration (Windows accepts NULL). */
    IDirect3DDevice9_CreateVertexDeclaration(dev, out_elements, &out_decl);
    hr = IDirect3DDevice9_ProcessVertices(dev, 0, 0, 1, dst, out_decl, 0);

    IDirect3DVertexBuffer9_Lock(dst, 0, 0, (void **)&v, D3DLOCK_READONLY);
    memcpy(out, v, sizeof(out));
    IDirect3DVertexBuffer9_Unlock(dst);

    if (!memcmp(&out[0], &sentinel, sizeof(sentinel)) && !memcmp(&out[1], &sentinel, sizeof(sentinel)))
        return report(1, "ProcessVertices hr %#lx, output untouched (refused)", hr);
    return report(SUCCEEDED(hr) && out[0] > 7.5f && out[0] < 8.5f && out[1] > 7.5f && out[1] < 8.5f,
                  "ProcessVertices hr %#lx, position (%g, %g) (expected (8, 8))", hr, out[0], out[1]);
}
