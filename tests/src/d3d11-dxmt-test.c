/* BO2's renderer: Direct3D 11 through DXMT (CX_GRAPHICS_BACKEND=dxmt), as
 * a 32-bit program like BO2. Creates a device, clears a 16x16 render target
 * to (0.25, 0.5, 0.75, 1), copies it to a staging texture and reads it back.
 * Expected: R 64, G 128, B 191, and the adapter description in the log.
 * Build: i686-w64-mingw32-gcc -O2 -o d3d11-dxmt-test.exe d3d11-dxmt-test.c -ld3d11 -ldxgi -luuid */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include "aio_test.h"

int main(void)
{
    static const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    D3D11_TEXTURE2D_DESC desc = {16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *rt, *staging;
    ID3D11RenderTargetView *rtv;
    IDXGIDevice *dxgi_dev;
    IDXGIAdapter *adapter;
    DXGI_ADAPTER_DESC adesc;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D_FEATURE_LEVEL level;
    DWORD px;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (FAILED(hr)) return report(0, "D3D11CreateDevice failed: %#lx", hr);
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev))
            && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &adesc)))
        printf("adapter: %ls, feature level %#x\n", adesc.Description, level);

    ID3D11Device_CreateTexture2D(dev, &desc, NULL, &rt);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &desc, NULL, &staging);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colour);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    if (FAILED(hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)))
        return report(0, "Map failed: %#lx", hr);
    px = ((DWORD *)((BYTE *)map.pData + 8 * map.RowPitch))[8];
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    /* R8G8B8A8 in memory: R is the low byte. */
    return report((px & 0xff) >= 62 && (px & 0xff) <= 66 && ((px >> 8) & 0xff) >= 126 && ((px >> 8) & 0xff) <= 130
                  && ((px >> 16) & 0xff) >= 189 && ((px >> 16) & 0xff) <= 193,
                  "cleared pixel R %lu G %lu B %lu (expected ~64, ~128, ~191)", px & 0xff, (px >> 8) & 0xff, (px >> 16) & 0xff);
}
