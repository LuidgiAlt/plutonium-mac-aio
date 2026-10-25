/* Shows what a game sees through XInput (xinput1_3, like BO2) for a number
 * of seconds: connected slots, then every change of buttons, sticks and
 * triggers.  Build: i686-w64-mingw32-gcc -O2 -o xinput-test.exe xinput-test.c -lsetupapi -lhid
 * Run in the prefix:  wine xinput-test.exe [seconds]                         */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "aio_test.h"

/* List the HID devices Wine presents, with vendor/product and top-level usage. */
static void list_hid(void)
{
    GUID guid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    char buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + MAX_PATH];
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (void *)buffer;
    DWORD i;

    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &iface); i++)
    {
        HIDD_ATTRIBUTES attr = {sizeof(attr)};
        PHIDP_PREPARSED_DATA pre;
        HIDP_CAPS caps = {0};
        HANDLE h;

        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buffer), NULL, NULL)) continue;
        h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            printf("hid: read/write open (as XInput does) failed, error %lu: %s\n", GetLastError(), detail->DevicePath);
            h = CreateFileA(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h != INVALID_HANDLE_VALUE)
        {
            HidD_GetAttributes(h, &attr);
            if (HidD_GetPreparsedData(h, &pre)) { HidP_GetCaps(pre, &caps); HidD_FreePreparsedData(pre); }
            CloseHandle(h);
        }
        printf("hid: %04x:%04x usage %02x:%02x inputs %u buttons-caps %u value-caps %u  %s\n",
               attr.VendorID, attr.ProductID, caps.UsagePage, caps.Usage, caps.InputReportByteLength,
               caps.NumberInputButtonCaps, caps.NumberInputValueCaps, detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
}

typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } PAD;
typedef struct { DWORD packet; PAD pad; } STATE;
typedef DWORD (WINAPI *GETSTATE)(DWORD, STATE *);

static const char *names[16] = { "Up", "Down", "Left", "Right", "Start", "Back", "LS", "RS",
                                 "LB", "RB", "Guide", "?", "A", "B", "X", "Y" };

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20, slot, i;
    HMODULE xi = LoadLibraryA("xinput1_3.dll");
    GETSTATE get = xi ? (GETSTATE)GetProcAddress(xi, "XInputGetState") : NULL;
    STATE last[4] = {0}, s;
    DWORD connected[4], end;

    list_hid();
    if (!get) return report(0, "cannot load xinput1_3.dll");
    for (slot = 0; slot < 4; slot++)
    {
        connected[slot] = get(slot, &s) == ERROR_SUCCESS;
        printf("slot %d: %s\n", slot, connected[slot] ? "CONNECTED" : "-");
        if (connected[slot]) last[slot] = s;
    }
    fflush(stdout);
    end = GetTickCount() + seconds * 1000;
    while (GetTickCount() < end)
    {
        for (slot = 0; slot < 4; slot++)
        {
            if (get(slot, &s) != ERROR_SUCCESS || s.packet == last[slot].packet) continue;
            if (memcmp(&s.pad, &last[slot].pad, sizeof(PAD)))
            {
                printf("slot %d: LX %6d LY %6d RX %6d RY %6d LT %3u RT %3u buttons:", slot,
                       s.pad.lx, s.pad.ly, s.pad.rx, s.pad.ry, s.pad.lt, s.pad.rt);
                for (i = 0; i < 16; i++) if (s.pad.b & (1 << i)) printf(" %s", names[i]);
                printf("\n");
                fflush(stdout);
            }
            last[slot] = s;
        }
        Sleep(30);
    }
    for (slot = 0; slot < 4; slot++) if (connected[slot]) return report(1, "slot %d connected", slot);
    return skip("no controller connected");
}
