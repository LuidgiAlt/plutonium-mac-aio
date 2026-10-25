/* Managed textures uploaded from their system memory copy (patch 0016),
 * each drawn full-screen with point sampling and read back:
 *   partial: an 8x8 A8R8G8B8 texture uploaded blue, then only its left half
 *            locked and filled red: left red, right blue
 *   mip1:    level 1 of a two-level texture (green) sampled with MAXMIPLEVEL 1
 *            (skipped with AIO_NO_MAXMIPLEVEL=1: wined3d's Vulkan renderer
 *            ignores MAXMIPLEVEL, also without these patches)
 *   dxt1:    8x8 DXT1, top block row red, bottom block row blue
 * (ATI2N, whose pitch counts in blocks: d3d9-ati2n-test.)
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-managed-formats-test.exe d3d9-managed-formats-test.c -ld3d9 -ld3dx9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"
#include "d3d9-common.h"

static void draw(IDirect3DTexture9 *tex)
{
    struct { float x, y, z, rhw, u, v; } quad[4] =
        {{-0.5f,-0.5f,0,1,0,0},{15.5f,-0.5f,0,1,1,0},{-0.5f,15.5f,0,1,0,1},{15.5f,15.5f,0,1,1,1}};
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    IDirect3DDevice9_EndScene(dev);
}

static void fill(IDirect3DTexture9 *tex, UINT level, const RECT *rect, DWORD colour, int width, int height)
{
    D3DLOCKED_RECT lr;
    int x, y;
    IDirect3DTexture9_LockRect(tex, level, &lr, rect, 0);
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = colour;
    IDirect3DTexture9_UnlockRect(tex, level);
}

/* One BC block row (two 4x4 blocks) of a compressed texture. */
static void fill_blocks(IDirect3DTexture9 *tex, const BYTE *block, int block_size)
{
    D3DLOCKED_RECT lr;
    int row, b;
    IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
    for (row = 0; row < 2; row++)
        for (b = 0; b < 2; b++)
            memcpy((BYTE *)lr.pBits + row * lr.Pitch + b * block_size, block + row * block_size, block_size);
    IDirect3DTexture9_UnlockRect(tex, 0);
}

int main(void)
{
    IDirect3DTexture9 *tex;
    RECT left = {0, 0, 4, 8};
    DWORD l, r, top, bottom;
    char result[512] = "";
    int ok = 1;

    if (!create_device()) return report(0, "CreateDevice failed");
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);

    /* partial */
    IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    fill(tex, 0, NULL, 0xff0000ff, 8, 8);
    draw(tex);
    fill(tex, 0, &left, 0xffff0000, 4, 8);
    draw(tex);
    l = read_pixel(2, 8) & 0xffffff; r = read_pixel(13, 8) & 0xffffff;
    if (l != 0xff0000 || r != 0x0000ff) ok = 0;
    sprintf(result + strlen(result), "partial %06lx/%06lx ", l, r);
    IDirect3DTexture9_Release(tex);

    /* mip1 */
    if (!GetEnvironmentVariableA("AIO_NO_MAXMIPLEVEL", NULL, 0))
    {
    IDirect3DDevice9_CreateTexture(dev, 8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    fill(tex, 0, NULL, 0xffff0000, 8, 8);
    fill(tex, 1, NULL, 0xff00ff00, 4, 4);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 1);
    draw(tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
    l = read_pixel(8, 8) & 0xffffff;
    if (l != 0x00ff00) ok = 0;
    sprintf(result + strlen(result), "mip1 %06lx ", l);
    IDirect3DTexture9_Release(tex);
    }

    /* dxt1: colour0 = colour1, all indices 0 */
    {
        static const BYTE blocks[16] = {0x00,0xf8, 0x00,0xf8, 0,0,0,0,   0x1f,0x00, 0x1f,0x00, 0,0,0,0};
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &tex, NULL)))
        {
            fill_blocks(tex, blocks, 8);
            draw(tex);
            top = read_pixel(8, 3) & 0xffffff; bottom = read_pixel(8, 12) & 0xffffff;
            if (!rgb_near(top, 255, 0, 0) || !rgb_near(bottom, 0, 0, 255)) ok = 0;
            sprintf(result + strlen(result), "dxt1 %06lx/%06lx ", top, bottom);
            IDirect3DTexture9_Release(tex);
        }
    }

    return report(ok, "%s(expected partial ff0000/0000ff, mip1 00ff00, dxt1 red/blue)", result);
}
