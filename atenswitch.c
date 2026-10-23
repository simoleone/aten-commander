// atenswitch – switch an ATEN US3342 KVM to its other host.
//
// Sends a vendor SCSI command to the KVM's Genesys Logic control device
// (USB 05E3:0627). See PROTOCOL.md.
//
// SPDX-License-Identifier: MIT

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_VID 0x05E3
#define GL_PID 0x0627

#define GL_OPCODE        0xF2
#define GL_CMD_SWITCH    0x07
#define GL_SWITCH_TOGGLE 0x80

#define CDB_LEN      16
#define INQUIRY_LEN  72
#define TIMEOUT_MS   6000

typedef struct {
    IOCFPlugInInterface **plugin;
    MMCDeviceInterface **mmc;
    SCSITaskDeviceInterface **dev;
    bool exclusive;
} GlDevice;

// Fill in byte 1 so that all 16 CDB bytes sum to 0 (mod 256).
static void gl_checksum(UInt8 cdb[CDB_LEN]) {
    UInt8 sum = 0;
    cdb[1] = 0;
    for (int i = 0; i < CDB_LEN; i++) sum += cdb[i];
    cdb[1] = (UInt8)-sum;
}

static void print_bytes(const char *label, const UInt8 *buf, size_t len) {
    printf("%s", label);
    for (size_t i = 0; i < len; i++) printf("%02X ", buf[i]);
    printf("\n");
}

static int usb_id_property(io_service_t svc, CFStringRef key) {
    int value = -1;
    CFTypeRef ref = IORegistryEntrySearchCFProperty(
        svc, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (ref) {
        if (CFGetTypeID(ref) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)ref, kCFNumberIntType, &value);
        CFRelease(ref);
    }
    return value;
}

// Run one SCSI task. dir is a kSCSIDataTransfer_* value.
static IOReturn exec_cdb(GlDevice *gl, const UInt8 *cdb, UInt8 cdb_len,
                         void *buf, UInt64 buf_len, UInt8 dir,
                         UInt64 *transferred, SCSITaskStatus *status) {
    SCSITaskInterface **task = (*gl->dev)->CreateSCSITask(gl->dev);
    if (!task) return kIOReturnNoMemory;

    SCSI_Sense_Data sense = {0};
    SCSITaskSGElement sg = { .address = (mach_vm_address_t)(uintptr_t)buf, .length = buf_len };
    IOReturn kr = (*task)->SetCommandDescriptorBlock(task, (UInt8 *)cdb, cdb_len);
    if (kr == kIOReturnSuccess)
        kr = (*task)->SetScatterGatherEntries(task, buf ? &sg : NULL, buf ? 1 : 0, buf_len, dir);
    if (kr == kIOReturnSuccess)
        kr = (*task)->SetTimeoutDuration(task, TIMEOUT_MS);
    if (kr == kIOReturnSuccess)
        kr = (*task)->ExecuteTaskSync(task, &sense, status, transferred);
    (*task)->Release(task);
    return kr;
}

// Vendor INQUIRY (12 00 00 55 48 00). A Genesys bridge answers with
// "GENE" at offset 0x28.
static bool gl_inquiry(GlDevice *gl, UInt8 out[INQUIRY_LEN], UInt64 *len) {
    UInt8 cdb[6] = { 0x12, 0x00, 0x00, 0x55, 0x48, 0x00 };
    SCSITaskStatus status = kSCSITaskStatus_No_Status;
    memset(out, 0, INQUIRY_LEN);
    IOReturn kr = exec_cdb(gl, cdb, sizeof cdb, out, INQUIRY_LEN,
                           kSCSIDataTransfer_FromTargetToInitiator, len, &status);
    if (kr != kIOReturnSuccess || status != kSCSITaskStatus_GOOD) {
        fprintf(stderr, "INQUIRY failed: kr=0x%08x status=%d\n", kr, status);
        return false;
    }
    return memmem(out + 0x24, INQUIRY_LEN - 0x24, "GENE", 4) != NULL;
}

static void gl_close(GlDevice *gl) {
    if (gl->dev) {
        if (gl->exclusive) (*gl->dev)->ReleaseExclusiveAccess(gl->dev);
        (*gl->dev)->Release(gl->dev);
    }
    if (gl->mmc) (*gl->mmc)->Release(gl->mmc);
    if (gl->plugin) IODestroyPlugInInterface(gl->plugin);
    memset(gl, 0, sizeof *gl);
}

static bool gl_open(GlDevice *gl) {
    memset(gl, 0, sizeof *gl);

    // Non-disk SCSI devices publish SCSITaskDeviceCategory = SCSITaskAuthoringDevice.
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR(kIOPropertySCSITaskDeviceCategory),
                         CFSTR(kIOPropertySCSITaskAuthoringDevice));
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);

    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed\n");
        return false;
    }

    bool found = false, matched = false;
    io_service_t svc;
    while (!found && (svc = IOIteratorNext(iter))) {
        int vid = usb_id_property(svc, CFSTR("idVendor"));
        int pid = usb_id_property(svc, CFSTR("idProduct"));
        if (vid == GL_VID && pid == GL_PID) {
            matched = true;
            // The device reports peripheral type 05 (CD/MMC), so macOS only hands out
            // the SCSITask interface via the MMC plug-in.
            SInt32 score = 0;
            IOReturn kr = IOCreatePlugInInterfaceForService(
                svc, kIOMMCDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
                &gl->plugin, &score);
            if (kr != kIOReturnSuccess || !gl->plugin) {
                fprintf(stderr, "IOCreatePlugInInterfaceForService failed: 0x%08x\n", kr);
                gl_close(gl);
            } else if ((*gl->plugin)->QueryInterface(
                           gl->plugin, CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID),
                           (LPVOID *)&gl->mmc) != S_OK || !gl->mmc) {
                fprintf(stderr, "QueryInterface(MMCDeviceInterface) failed\n");
                gl_close(gl);
            } else if (!(gl->dev = (*gl->mmc)->GetSCSITaskDeviceInterface(gl->mmc))) {
                fprintf(stderr, "GetSCSITaskDeviceInterface failed\n");
                gl_close(gl);
            } else if ((kr = (*gl->dev)->ObtainExclusiveAccess(gl->dev)) != kIOReturnSuccess) {
                fprintf(stderr, "ObtainExclusiveAccess failed: 0x%08x "
                                "(is Bezel X running?)\n", kr);
                gl_close(gl);
            } else {
                gl->exclusive = true;
                found = true;
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);

    if (!matched)
        fprintf(stderr, "KVM control device %04x:%04x not found "
                        "(it needs a USB 3 link to the KVM)\n", GL_VID, GL_PID);
    return found;
}

static int cmd_probe(void) {
    GlDevice gl;
    if (!gl_open(&gl)) return 1;
    UInt8 inq[INQUIRY_LEN];
    UInt64 len = 0;
    bool gene = gl_inquiry(&gl, inq, &len);
    printf("INQUIRY returned %llu bytes, GENE signature %s\n", len, gene ? "present" : "MISSING");
    printf("vendor  : %.8s\nproduct : %.16s\nrev     : %.4s\n", inq + 8, inq + 16, inq + 32);
    for (int off = 0; off < INQUIRY_LEN; off += 16) {
        char label[16];
        snprintf(label, sizeof label, "  %02X: ", off);
        print_bytes(label, inq + off, off + 16 <= INQUIRY_LEN ? 16 : INQUIRY_LEN - off);
    }
    gl_close(&gl);
    return gene ? 0 : 1;
}

static int cmd_switch(UInt8 arg, bool dry_run) {
    UInt8 cdb[CDB_LEN] = { GL_OPCODE, 0, GL_CMD_SWITCH, arg };
    gl_checksum(cdb);
    if (dry_run) {
        print_bytes("CDB: ", cdb, CDB_LEN);
        return 0;
    }

    GlDevice gl;
    if (!gl_open(&gl)) return 1;

    UInt8 inq[INQUIRY_LEN];
    UInt64 len = 0;
    if (!gl_inquiry(&gl, inq, &len)) {
        fprintf(stderr, "device did not identify as a Genesys host bridge, not sending\n");
        gl_close(&gl);
        return 1;
    }

    SCSITaskStatus status = kSCSITaskStatus_No_Status;
    UInt64 transferred = 0;
    IOReturn kr = exec_cdb(&gl, cdb, CDB_LEN, NULL, 0, kSCSIDataTransfer_NoDataTransfer,
                           &transferred, &status);
    gl_close(&gl);
    if (kr != kIOReturnSuccess || status != kSCSITaskStatus_GOOD) {
        fprintf(stderr, "switch command failed: kr=0x%08x status=%d\n", kr, status);
        return 1;
    }
    return 0;
}

static void usage(FILE *out, const char *argv0) {
    fprintf(out,
            "usage: %s [-n] [switch [arg]]\n"
            "       %s probe\n"
            "\n"
            "  switch [arg]  toggle the KVM to its other host (default; arg defaults to 0x80)\n"
            "  probe         check the control device and dump its INQUIRY data\n"
            "  -n            dry run: print the command bytes, send nothing\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    int i = 1;
    bool dry_run = false;
    if (i < argc && (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)) {
        usage(stdout, argv[0]);
        return 0;
    }
    if (i < argc && strcmp(argv[i], "-n") == 0) { dry_run = true; i++; }

    if (i >= argc) return cmd_switch(GL_SWITCH_TOGGLE, dry_run);
    if (strcmp(argv[i], "probe") == 0 && i + 1 == argc && !dry_run) return cmd_probe();
    if (strcmp(argv[i], "switch") == 0 && i + 2 >= argc) {
        unsigned long arg = GL_SWITCH_TOGGLE;
        if (i + 1 < argc) {
            char *end;
            arg = strtoul(argv[i + 1], &end, 0);
            if (*end || arg > 0xFF) { usage(stderr, argv[0]); return 2; }
        }
        return cmd_switch((UInt8)arg, dry_run);
    }
    usage(stderr, argv[0]);
    return 2;
}
