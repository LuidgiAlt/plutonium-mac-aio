/* A managed ATI2N texture (patch 0016 uploads managed textures from their
 * system memory copy; ATI2N's pitch counts in blocks): 8x8, the top block
 * row 0x20 and the bottom block row 0xc0 in both channels, drawn full-screen
 * with point sampling. Run on wined3d's OpenGL renderer (its Vulkan renderer
 * has no ATI2N). Expected: top R G 0x20, bottom R G 0xc0.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-ati2n-test.exe d3d9-ati2n-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

#define ATI2N ((D3DFORMAT)MAKEFOURCC('A','T','I','2'))

int main(void)
{
    /* Per block: two BC4 channels, both endpoints equal, all indices 0. */
    static const BYTE rows[2][16] =
    {
        {0x20,0x20,0,0,0,0,0,0, 0x20,0x20,0,0,0,0,0,0},
        {0xc0,0xc0,0,0,0,0,0,0, 0xc0,0xc0,0,0,0,0,0,0},
    };
    struct { float x, y, z, rhw, u, v; } quad[4] =
        {{-0.5f,-0.5f,0,1,0,0},{15.5f,-0.5f,0,1,1,0},{-0.5f,15.5f,0,1,0,1},{15.5f,15.5f,0,1,1,1}};
    IDirect3DTexture9 *tex;
    D3DLOCKED_RECT lr;
    DWORD top, bottom;
    int row, b;

    if (!create_device()) return report(0, "CreateDevice failed");
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, ATI2N, D3DPOOL_MANAGED, &tex, NULL)))
        return skip("no ATI2N textures");
    IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
    for (row = 0; row < 2; row++)
        for (b = 0; b < 2; b++)
            /* ATI2N's reported pitch is one byte per pixel (Direct3D 9's "broken
             * pitch"); its blocks are contiguous, two per block row here. */
            memcpy((BYTE *)lr.pBits + row * 2 * 16 + b * 16, rows[row], 16);
    IDirect3DTexture9_UnlockRect(tex, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
    top = read_pixel(8, 3) & 0xffffff;
    bottom = read_pixel(8, 12) & 0xffffff;
    return report(channel_near(top, 16, 0x20) && channel_near(top, 8, 0x20)
                  && channel_near(bottom, 16, 0xc0) && channel_near(bottom, 8, 0xc0),
                  "top %06lx (expected 2020..), bottom %06lx (expected c0c0..)", top, bottom);
}
