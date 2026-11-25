/* Publish the S49 MK3 vendor interface to SetupDi and prove CreateFile
 * can open it. Wineusb enumerates MI_03 but never registers a device
 * interface, so the hardware service has nothing to open. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

static const WCHAR usb_guid[] = L"{A5DCBF10-6530-11D2-901F-00C04FB951ED}";
/* GUID the hardware service actually enumerates. Seen in its setupapi trace. */
static const WCHAR ni_guid[] = L"{9CB5BE68-52CA-4F04-A394-090391A15E51}";

static void publish_guid(const WCHAR *inst, const WCHAR *guid)
{
    WCHAR devinst[400], link[500], class_key[700], hash_key[720], ctrl_key[740], dosname[500];
    swprintf(devinst, ARRAYSIZE(devinst), L"USB\\VID_17CC&PID_2100&MI_03\\%s", inst);
    swprintf(link, ARRAYSIZE(link),
            L"\\\\?\\USB#VID_17CC&PID_2100&MI_03#%s#%s", inst, guid);
    swprintf(class_key, ARRAYSIZE(class_key),
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses\\%s\\##?#USB#VID_17CC&PID_2100&MI_03#%s#%s",
            guid, inst, guid);
    swprintf(hash_key, ARRAYSIZE(hash_key), L"%s\\#", class_key);
    swprintf(ctrl_key, ARRAYSIZE(ctrl_key), L"%s\\Control", hash_key);
    /* This has to be the guid being published. The generic USB guid leaves
     * the NI path with no DOS device, so CreateFile returns error 2 and the
     * hardware service reports that the bulk pipes could not be connected. */
    swprintf(dosname, ARRAYSIZE(dosname), L"USB#VID_17CC&PID_2100&MI_03#%s#%s", inst, guid);

    HKEY key;
    DWORD linked = 1;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        wprintf(L"class key failed %lu\n", GetLastError());
        return;
    }
    RegSetValueExW(key, L"DeviceInstance", 0, REG_SZ, (BYTE *)devinst,
            (DWORD)((wcslen(devinst) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, hash_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        RegSetValueExW(key, L"SymbolicLink", 0, REG_SZ, (BYTE *)link,
                (DWORD)((wcslen(link) + 1) * sizeof(WCHAR)));
        RegCloseKey(key);
    }
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, ctrl_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        RegSetValueExW(key, L"Linked", 0, REG_DWORD, (BYTE *)&linked, sizeof linked);
        RegCloseKey(key);
    }

    /* ClassGUID must be a braced GUID or SetupDi skips the interface. */
    WCHAR enum_key[400];
    swprintf(enum_key, ARRAYSIZE(enum_key), L"SYSTEM\\CurrentControlSet\\Enum\\%s", devinst);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, enum_key, 0, KEY_READ | KEY_SET_VALUE, &key)) {
        WCHAR class_guid[64];
        DWORD n = sizeof class_guid, type = 0;
        class_guid[0] = 0;
        RegQueryValueExW(key, L"ClassGUID", NULL, &type, (BYTE *)class_guid, &n);
        if (class_guid[0] != L'{' || wcslen(class_guid) < 38) {
            static const WCHAR usb_class[] = L"{36FC9E60-C465-11CF-8056-444553540000}";
            RegSetValueExW(key, L"ClassGUID", 0, REG_SZ, (BYTE *)usb_class,
                    (DWORD)((wcslen(usb_class) + 1) * sizeof(WCHAR)));
            wprintf(L"set ClassGUID on %s\n", devinst);
        } else {
            wprintf(L"ClassGUID %s\n", class_guid);
        }
        RegCloseKey(key);
    } else {
        wprintf(L"enum key missing %s (%lu)\n", devinst, GetLastError());
    }

    DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
            dosname, L"\\Device\\Null");
    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH, dosname, L"\\Device\\Null"))
        wprintf(L"DefineDosDevice failed %lu\n", GetLastError());

    HANDLE file = CreateFileW(link, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, NULL);
    wprintf(L"CreateFile %s -> %p err %lu\n", link, file, GetLastError());
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

static void publish(const WCHAR *inst)
{
    publish_guid(inst, usb_guid);
    publish_guid(inst, ni_guid);
}

/* A replug leaves the previous bus/port interface marked present. SetupDi
 * would keep offering that dead path, so clear those before publishing. */
static void unlink_guid(const WCHAR *guid)
{
    WCHAR root_path[300];
    HKEY root;
    DWORD i = 0;

    swprintf(root_path, ARRAYSIZE(root_path),
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses\\%s", guid);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path, 0, KEY_ENUMERATE_SUB_KEYS, &root))
        return;
    for (;;) {
        WCHAR name[700];
        DWORD len = ARRAYSIZE(name);
        WCHAR ctrl[780];
        HKEY key;
        DWORD linked = 0;
        if (RegEnumKeyExW(root, i++, name, &len, NULL, NULL, NULL, NULL))
            break;
        if (!wcsstr(name, L"VID_17CC&PID_2100&MI_03"))
            continue;
        swprintf(ctrl, ARRAYSIZE(ctrl), L"%s\\%s\\#\\Control", root_path, name);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ctrl, 0, KEY_SET_VALUE, &key))
            continue;
        RegSetValueExW(key, L"Linked", 0, REG_DWORD, (BYTE *)&linked, sizeof linked);
        RegCloseKey(key);
        wprintf(L"unlinked %s\n", name);
    }
    RegCloseKey(root);
}

/* S49_USB is the sysfs name, such as 8-4. Wine's instance id ends with &8&4. */
static int instance_matches(const WCHAR *inst)
{
    char spec[80];
    WCHAR suffix[90];
    WCHAR *w = suffix;
    DWORD n;
    size_t slen, ilen;

    n = GetEnvironmentVariableA("S49_USB", spec, sizeof spec);
    if (!n || n >= sizeof spec)
        return 1;
    *w++ = L'&';
    for (DWORD i = 0; spec[i] && w + 1 < suffix + ARRAYSIZE(suffix); i++) {
        char c = spec[i];
        if (c == '-' || c == '.')
            *w++ = L'&';
        else if (c >= '0' && c <= '9')
            *w++ = (WCHAR)c;
    }
    *w = 0;
    slen = wcslen(suffix);
    ilen = wcslen(inst);
    if (slen < 2 || ilen < slen)
        return 0;
    return wcscmp(inst + ilen - slen, suffix) == 0;
}

static void list_one(const GUID *guid, const WCHAR *name)
{
    HDEVINFO info = SetupDiGetClassDevsW(guid, NULL, NULL,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    wprintf(L"SetupDi %s %p err %lu\n", name, info, GetLastError());
    if (info == INVALID_HANDLE_VALUE)
        return;
    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, NULL, guid, i, &iface); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = calloc(1, need);
        detail->cbSize = sizeof *detail;
        SP_DEVINFO_DATA dev;
        dev.cbSize = sizeof dev;
        if (SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, need, NULL, &dev))
            wprintf(L"PATH %s\n", detail->DevicePath);
        else
            wprintf(L"detail err %lu\n", GetLastError());
        free(detail);
    }
    wprintf(L"enum done err %lu\n", GetLastError());
    SetupDiDestroyDeviceInfoList(info);
}

static void list_interfaces(void)
{
    list_one(&GUID_DEVINTERFACE_USB_DEVICE, L"USB_DEVICE");
    static const GUID ni = { 0x9CB5BE68, 0x52CA, 0x4F04, { 0xA3, 0x94, 0x09, 0x03, 0x91, 0xA1, 0x5E, 0x51 } };
    list_one(&ni, L"NI");
}

static void test_winusb(void)
{
    HMODULE dll = LoadLibraryW(L"winusb.dll");
    wprintf(L"LoadLibrary winusb %p err %lu\n", dll, GetLastError());
    if (!dll)
        return;
    typedef BOOL (WINAPI *init_fn)(HANDLE, PWINUSB_INTERFACE_HANDLE);
    typedef BOOL (WINAPI *desc_fn)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, USHORT, PUCHAR, ULONG, PULONG);
    typedef BOOL (WINAPI *qi_fn)(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
    typedef BOOL (WINAPI *qp_fn)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, PWINUSB_PIPE_INFORMATION);
    typedef BOOL (WINAPI *free_fn)(WINUSB_INTERFACE_HANDLE);
    init_fn init = (init_fn)GetProcAddress(dll, "WinUsb_Initialize");
    desc_fn get_desc = (desc_fn)GetProcAddress(dll, "WinUsb_GetDescriptor");
    qi_fn query_if = (qi_fn)GetProcAddress(dll, "WinUsb_QueryInterfaceSettings");
    qp_fn query_pipe = (qp_fn)GetProcAddress(dll, "WinUsb_QueryPipe");
    free_fn free_usb = (free_fn)GetProcAddress(dll, "WinUsb_Free");
    if (!init || !get_desc || !query_if || !query_pipe) {
        wprintf(L"exports missing\n");
        return;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    WINUSB_INTERFACE_HANDLE usb = NULL;
    if (!init(nul, &usb)) {
        wprintf(L"WinUsb_Initialize failed %lu\n", GetLastError());
        CloseHandle(nul);
        return;
    }
    unsigned char buf[255];
    ULONG n = 0;
    memset(buf, 0, sizeof buf);
    if (!get_desc(usb, 3, 2, 0x0409, buf, sizeof buf, &n))
        wprintf(L"product descriptor failed %lu\n", GetLastError());
    else {
        wprintf(L"product descriptor %lu bytes:", n);
        for (ULONG i = 0; i < n && i < 64; i++)
            wprintf(L" %02x", buf[i]);
        wprintf(L"\n");
    }
    USB_INTERFACE_DESCRIPTOR id;
    memset(&id, 0, sizeof id);
    if (!query_if(usb, 0, &id))
        wprintf(L"QueryInterfaceSettings failed %lu\n", GetLastError());
    else
        wprintf(L"iface num=%u eps=%u class=%u sub=%u proto=%u\n",
                id.bInterfaceNumber, id.bNumEndpoints, id.bInterfaceClass,
                id.bInterfaceSubClass, id.bInterfaceProtocol);
    for (UCHAR i = 0; i < 8; i++) {
        WINUSB_PIPE_INFORMATION pipe;
        if (!query_pipe(usb, 0, i, &pipe))
            break;
        wprintf(L"pipe[%u] id=0x%x type=%u mps=%u\n", i, pipe.PipeId, pipe.PipeType, pipe.MaximumPacketSize);
    }
    if (free_usb)
        free_usb(usb);
    CloseHandle(nul);
}

int wmain(void)
{
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_17CC&PID_2100&MI_03",
            0, KEY_READ, &root)) {
        wprintf(L"MI_03 key missing %lu\n", GetLastError());
        return 1;
    }
    WCHAR insts[8][200];
    DWORD i = 0, len;
    int ninst = 0;
    int found = 0;
    unlink_guid(usb_guid);
    unlink_guid(ni_guid);
    for (;;) {
        len = ARRAYSIZE(insts[0]);
        if (ninst >= 8 || RegEnumKeyExW(root, i++, insts[ninst], &len, NULL, NULL, NULL, NULL))
            break;
        wprintf(L"instance %s\n", insts[ninst]);
        ninst++;
    }
    RegCloseKey(root);
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (int n = 0; n < ninst; n++) {
            if (pass == 0 && !instance_matches(insts[n])) {
                wprintf(L"skip %s\n", insts[n]);
                continue;
            }
            if (pass == 1)
                wprintf(L"no port match, publishing %s\n", insts[n]);
            publish(insts[n]);
            found = 1;
        }
    }
    if (!found)
        return 1;
    list_interfaces();
    test_winusb();
    return 0;
}
