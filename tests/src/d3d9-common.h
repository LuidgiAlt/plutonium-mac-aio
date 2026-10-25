/* Shared by the Direct3D 9 tests: a 16x16 device and render target,
 * shader compilation and pixel read-back. */
#include <d3dx9.h>
#include <stdlib.h>

static IDirect3DDevice9 *dev;
static IDirect3DSurface9 *rt, *sysmem;

static int create_device(void)
{
    HWND hwnd = CreateWindowA("STATIC", "t", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};

    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return 0;
    IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    return 1;
}

static DWORD *compile_hlsl(const char *src, const char *profile)
{
    ID3DXBuffer *code, *errors = NULL;
    if (FAILED(D3DXCompileShader(src, strlen(src), NULL, NULL, "main", profile, 0, &code, &errors, NULL)))
    {
        printf("FAIL %s: compile failed: %s\n", TEST_NAME, errors ? (char *)ID3DXBuffer_GetBufferPointer(errors) : "?");
        exit(1);
    }
    return ID3DXBuffer_GetBufferPointer(code);
}

static DWORD *assemble(const char *src)
{
    ID3DXBuffer *code, *errors = NULL;
    if (FAILED(D3DXAssembleShader(src, strlen(src), NULL, NULL, 0, &code, &errors)))
    {
        printf("FAIL %s: assemble failed: %s\n", TEST_NAME, errors ? (char *)ID3DXBuffer_GetBufferPointer(errors) : "?");
        exit(1);
    }
    return ID3DXBuffer_GetBufferPointer(code);
}

static DWORD read_pixel(int x, int y)
{
    D3DLOCKED_RECT lr;
    DWORD px;
    IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem);
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    px = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    IDirect3DSurface9_UnlockRect(sysmem);
    return px;
}
