/* The network under the PC editor's link (port/port_stereo_link.c): the SOC
 * service, brought up only while the link is on. The updater brings it up
 * on its own for a check or a download; the two cannot hold it at once, so
 * the link simply fails to start while an update runs and tries again. */
#include "port_stereo_link.h"

#include <3ds.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdlib.h>
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
