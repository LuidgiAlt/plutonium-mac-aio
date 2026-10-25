/* Prints the Direct3D 9 caps a game sees (texture limits that some games check).
 * Build: i686-w64-mingw32-gcc -O2 -o d3d9-caps.exe d3d9-caps.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "aio_test.h"

int main(void)
{
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DCAPS9 caps;
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    if (FAILED(IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) { printf("GetDeviceCaps failed\n"); return 1; }
    printf("MaxSimultaneousTextures %lu\nMaxTextureBlendStages %lu\nTextureOpCaps 0x%08lx\n"
           "PixelShaderVersion %lu.%lu\nVertexShaderVersion %lu.%lu\n",
           caps.MaxSimultaneousTextures, caps.MaxTextureBlendStages, caps.TextureOpCaps,
           D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
           D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion));
    IDirect3D9_Release(d3d);
    /* Patch 0005: wined3d's Vulkan renderer reported 0 fixed-function textures. */
    return report(caps.MaxSimultaneousTextures >= 8 && caps.MaxTextureBlendStages >= 8,
                  "MaxSimultaneousTextures %lu, MaxTextureBlendStages %lu (expected >= 8)",
                  caps.MaxSimultaneousTextures, caps.MaxTextureBlendStages);
}
