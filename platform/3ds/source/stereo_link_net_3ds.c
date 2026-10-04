/* The network under the PC editor's link (port/port_stereo_link.c): the SOC
 * service, brought up only while the link is on. The updater brings it up
 * on its own for a check or a download; the two cannot hold it at once, so
 * the link simply fails to start while an update runs and tries again. */
#include "port_stereo_link.h"

#include <3ds.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOC_BUFFER_SIZE 0x100000

static void* sSocBuffer;
static bool sAc;

bool PortStereoLink_NetUp(uint32_t* address) {
    if (R_SUCCEEDED(acInit())) {
        sAc = true;
    }
    sSocBuffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if (sSocBuffer == NULL || R_FAILED(socInit(sSocBuffer, SOC_BUFFER_SIZE))) {
        free(sSocBuffer);
        sSocBuffer = NULL;
        if (sAc) {
            acExit();
            sAc = false;
        }
        return false;
    }
    /* gethostid is the address in network order, as the bytes of a word. */
    *address = (uint32_t)gethostid();
    return true;
}

void PortStereoLink_NetDown(void) {
    if (sSocBuffer != NULL) {
        socExit();
        free(sSocBuffer);
        sSocBuffer = NULL;
    }
    if (sAc) {
        acExit();
        sAc = false;
    }
}

/* Relaunch into another build: tell Luma's homebrew loader (hb:ldr, what the
 * Homebrew Launcher itself uses) which 3DSX to start next, then leave; the
 * loader starts it instead of the launcher. Only from the Homebrew Launcher. */
extern void Platform3DS_RequestQuit(void);

static Result HbldrCall(Handle handle, u32 command, const void* data, u32 size, int bufferId) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(command, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(size, bufferId);
    cmdbuf[2] = (u32)data;
    Result rc = svcSendSyncRequest(handle);
    return R_SUCCEEDED(rc) ? (Result)cmdbuf[1] : rc;
}

int PortStereoLink_Relaunch(const char* name) {
    char path[128], full[136];
    snprintf(path, sizeof(path), "/3ds/%s", name);
    snprintf(full, sizeof(full), "sdmc:%s", path);
    struct stat st;
    if (stat(full, &st) != 0) return 404;
    if (!envIsHomebrew()) return 409;
    Handle hbldr;
    if (R_FAILED(srvGetServiceHandle(&hbldr, "hb:ldr"))) return 409;
    /* argv as the launcher passes it: argc, then the path. */
    static u32 argv[64];
    memset(argv, 0, sizeof(argv));
    argv[0] = 1;
    snprintf((char*)&argv[1], sizeof(argv) - sizeof(u32), "%s", full);
    Result rc = HbldrCall(hbldr, 2, path, (u32)strlen(path) + 1, 0);
    if (R_SUCCEEDED(rc)) rc = HbldrCall(hbldr, 3, argv, sizeof(argv), 1);
    svcCloseHandle(hbldr);
    return R_SUCCEEDED(rc) ? 200 : 500;
}

void PortStereoLink_Quit(void) {
    Platform3DS_RequestQuit();
}
