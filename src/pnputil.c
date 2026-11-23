/* Prefix-local pnputil. Wine's builtin prints nothing for
 * "pnputil /enum-devices /class USB /relations", which is how
 * NIHardwareConnectionService discovers the keyboard. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_DEV 256

struct inst {
    char id[400];
    char devid[180];
    char inst[180];
};

static struct inst devs[MAX_DEV];
static int ndevs;

static void add_dev(const char *devid, const char *inst)
{
    if (ndevs >= MAX_DEV)
        return;
    struct inst *d = &devs[ndevs++];
    snprintf(d->devid, sizeof d->devid, "%s", devid);
    snprintf(d->inst, sizeof d->inst, "%s", inst);
    snprintf(d->id, sizeof d->id, "USB\\%s\\%s", devid, inst);
}

static int same_inst(const struct inst *a, const char *devid, const char *inst)
{
    return !strcmp(a->devid, devid) && !strcmp(a->inst, inst);
}

static void parent_of(const struct inst *d, char *out, size_t outlen)
{
    const char *mi = strstr(d->devid, "&MI_");
    if (!mi) {
        if (outlen)
            out[0] = 0;
        return;
    }
    char devid[160];
    snprintf(devid, sizeof devid, "%s", d->devid);
    devid[mi - d->devid] = 0;
    for (int i = 0; i < ndevs; i++) {
        if (same_inst(&devs[i], devid, d->inst)) {
            snprintf(out, outlen, "%s", devs[i].id);
            return;
        }
    }
    snprintf(out, outlen, "USB\\%s\\%s", devid, d->inst);
}

static int is_s49(const struct inst *d)
{
    return strstr(d->devid, "VID_17CC&PID_2100") || strstr(d->devid, "VID_17cc&PID_2100");
}

static void print_device(const struct inst *d)
{
    char parent[300];
    parent_of(d, parent, sizeof parent);
    const char *desc = "USB Device";
    const char *mfg = "(Standard USB Host Controller)";
    const char *driver = "usb.inf";
    if (is_s49(d)) {
        desc = strstr(d->devid, "&MI_03") ? "KONTROL S49 MK3" : "KONTROL S49 MK3";
        mfg = "Native Instruments";
        if (strstr(d->devid, "&MI_03"))
            driver = "winusb.inf";
        else if (!strstr(d->devid, "&MI_"))
            driver = "usbccgp.inf";
    }
    printf("Instance ID:                %s\n", d->id);
    printf("Device Description:         %s\n", desc);
    printf("Class Name:                 USB\n");
    printf("Class GUID:                 {36fc9e60-c465-11cf-8056-444553540000}\n");
    printf("Manufacturer Name:          %s\n", mfg);
    printf("Status:                     Started\n");
    printf("Driver Name:                %s\n", driver);
    if (parent[0])
        printf("Parent:                     %s\n", parent);
    int any = 0;
    for (int i = 0; i < ndevs; i++) {
        const char *mi = strstr(devs[i].devid, "&MI_");
        if (!mi || strcmp(devs[i].inst, d->inst))
            continue;
        char base[160];
        snprintf(base, sizeof base, "%s", devs[i].devid);
        base[mi - devs[i].devid] = 0;
        if (strcmp(base, d->devid))
            continue;
        if (!any)
            printf("Children:                   %s\n", devs[i].id);
        else
            printf("                            %s\n", devs[i].id);
        any = 1;
    }
    if (!any && !strstr(d->devid, "&MI_"))
        printf("Children:\n");
    printf("\n");
}

static void load_usb(void)
{
    HKEY usb;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB",
            0, KEY_READ, &usb))
        return;
    char devid[160];
    DWORD i = 0, devid_len;
    for (;;) {
        devid_len = sizeof devid;
        if (RegEnumKeyExA(usb, i++, devid, &devid_len, NULL, NULL, NULL, NULL))
            break;
        HKEY dev;
        if (RegOpenKeyExA(usb, devid, 0, KEY_READ, &dev))
            continue;
        char inst[160];
        DWORD j = 0, inst_len;
        for (;;) {
            inst_len = sizeof inst;
            if (RegEnumKeyExA(dev, j++, inst, &inst_len, NULL, NULL, NULL, NULL))
                break;
            add_dev(devid, inst);
        }
        RegCloseKey(dev);
    }
    RegCloseKey(usb);
}

static int has_arg(int argc, char **argv, const char *want)
{
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], want))
            return 1;
    }
    return 0;
}

static const char *arg_after(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcasecmp(argv[i], flag))
            return argv[i + 1];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    FILE *logf = fopen("Z:\\tmp\\pnputil-last.txt", "w");
    if (logf) {
        for (int i = 0; i < argc; i++)
            fprintf(logf, "argv[%d]=%s\n", i, argv[i]);
    }
    printf("Microsoft PnP Utility\n\n");
    if (!has_arg(argc, argv, "/enum-devices")) {
        if (logf)
            fclose(logf);
        return 0;
    }
    const char *class_name = arg_after(argc, argv, "/class");
    load_usb();
    if (logf)
        fprintf(logf, "devices=%d class=%s\n", ndevs, class_name ? class_name : "");
    for (int i = 0; i < ndevs; i++) {
        if (class_name && strcasecmp(class_name, "USB") && strcasecmp(class_name, "USBDevice"))
            continue;
        print_device(&devs[i]);
    }
    fflush(stdout);
    if (logf)
        fclose(logf);
    return 0;
}
