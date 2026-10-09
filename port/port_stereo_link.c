/**
 * @file port_stereo_link.c
 * @brief Stereoscopic 3D: the game as a small HTTP server for the PC editor.
 *
 * See port_stereo_link.h. One listening socket and a few clients, all
 * non-blocking and served from the game thread once a frame, so a request
 * sees the engine between frames and never races it. HTTP/1.0 with
 * "Connection: close": one request per connection, which is all fetch()
 * needs.
 *
 * GET /room answers this layout, little-endian:
 *
 *   0  "TMCR"            4  u16 version (1)   6  u8 area   7  u8 room
 *   8  u16 width, height, originX, originY (pixels; origin in the area)
 *   16 u16 bgcnt bottom, bgcnt top (0 when the layer is not shown)
 *   20 u16 tileset id, u16 cols, u16 rows (8x8 cells of the room)
 *   26 s16 scrollX, scrollY (camera, in the area)   30 u16 entity count
 *   32 per layer (bottom, then top):
 *        u16 mapData[64*64], u8 collision[64*64], u8 actTiles[64*64],
 *        u16 subTiles[2048*4]
 *      u8 vram[0x10000] (background tiles), u16 palette[256]
 *      per cell, 128 a row: s8 autoGround, s8 autoHeight, s8 autoHeightTop,
 *        u8 kind  (each a 128*128 block)
 *      u32 tileHash[2][64*64]
 *      entities: u8 kind, id, type, type2; s16 x, y, z (room pixels); s8 depth
 *        correction; u8 pad   (12 bytes each)
 *      "OAMS", u16 DISPCNT, u16 0, OAM[0x400] (screen coordinates at the
 *        camera above), OBJ tiles[0x8000], OBJ palette u16[256]
 *      "OAMX" (none or more, from POST /sweep): s16 scrollX, scrollY, u16
 *        DISPCNT, u16 0, OAM[0x400], OBJ tiles[0x8000] -- the sprites at one
 *        stop of a camera tour of the room
 *      "CHRS" (when known): u16 cols, rows; per layer: u8 seen[rows*cols],
 *        then the 32 bytes of 4bpp pixels of each cell as last on screen
 *        (cells whose graphics VRAM swaps as the camera moves)
 *      "RIDX", u16 drawIndex[2][64*64]: the tile set entry each map tile is
 *        drawn with (special tiles resolved)
 */
#include "port_stereo_link.h"
#include "port_stereo.h"
#include "port_stereo_edits.h"
#include "port_stereo_editor.h"

#include "global.h"
#include "area.h"
#include "entity.h"
#include "main.h"
#include "map.h"
#include "player.h"
#include "room.h"
#include "scroll.h"
#include "transitions.h"
#include "fade.h"
#include "fileselect.h"
#include "ui.h"
#include "structures.h"
#include "screen.h"
#include "save.h"
#include "port_gba_mem.h"
#include "port_rom.h"
#include "cpu/mode1.h"

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <strings.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PORT_STEREO_LINK_EDITOR_PAGE
#define PORT_STEREO_LINK_EDITOR_PAGE "romfs:/stereo_editor.html"
#endif

#define LINK_START_BUTTON 0x0008 /* KEYINPUT bit, active low */

/* Areas 0x00..0x8F, as port_rom.c resolves them. */
#define LINK_AREA_COUNT 0x90

enum {
    MAX_CLIENTS = 12,
    DRAIN_FRAMES = 30,
    MAX_REQUEST = 2 * 1024 * 1024,
    UPLOAD_PER_TICK = 64 * 1024,
    UPLOAD_STALL_FRAMES = 60 * 8,
    SEND_PER_TICK = 96 * 1024,
    CLIENT_TIMEOUT_FRAMES = 60 * 60,
    RECV_CHUNK = 32 * 1024,
    SIDE = PORT_STEREO_EDIT_SIDE,
    MAX_HIGHLIGHT = 4096,
};

typedef struct {
    int socket;
    char* request;
    size_t requestLength, requestCapacity;
    char* response;
    size_t responseLength, responseSent;
    unsigned age;
    /* Waiting for the renderer's copy of the eyes (GET /frame). */
    unsigned frameWait;
    /* POST /file goes to the card as it arrives: a 3DSX held whole in memory
     * starved the game (it froze the console). */
    FILE* upload;
    size_t uploadLeft, uploadDone;
    bool uploadFailed;
    char uploadMagic[4];
    char uploadPath[96];
    /* POST /file?...&quit=0 leaves the game running once the build is in. */
    bool uploadKeep;
    /* POST /launcher: the body is a new Homebrew Launcher (sdmc:/boot.3dsx). */
    bool uploadLauncher;
    /* Frames since the answer went out: the console's sockets drop what is
     * still unsent when closed at once, so the client closes first. */
    unsigned draining;
} Client;

static volatile bool sEnabled;
static bool sNetUp;
static bool sNetFailed;
static int sPort = PORT_STEREO_LINK_PORT;
static uint32_t sAddress;
static int sListen = -1;
static Client sClients[MAX_CLIENTS];
static unsigned sFrame;
static unsigned sQuitIn;
static bool sHighlightHidden;
/* Buttons pressed from the PC: which, for how many more frames. */
static uint32_t sInjectKeys;
static unsigned sInjectFrames, sInjectTotal;
/* Link walks through walls while an editor is open, unless the PC editor's
 * "through walls" is unticked; kept until the game quits, whatever room. */
static bool sNoclipWanted = true;
/* Asleep (lid shut): the link is down; after waking, frames to wait for the
 * Wi-Fi before bringing it up again. */
static volatile bool sAsleep;
static unsigned sWakeWait;
/* The network was up when the console went to sleep: restart it (outside
 * the APT hook) before listening again. */
static bool sNetResetPending;
static void CopyOamWithoutLink(u16* out);
static unsigned HeightsRevision(void);
extern int Port_Widescreen_GameplayViewWidth(void);
extern int Port_Widescreen_GameplayViewHeight(void);
static unsigned sQuitAt;
enum { QUIT_AFTER_UPLOAD_FRAMES = 60 * 3 };
/* Time (CPU ticks) a paused frame may spend taking an upload, so the
 * progress screen stays lively: the card write is the slow part. */
extern unsigned long long Platform3DS_SystemTick(void);
#define UPLOAD_TICKS_PER_FRAME (268111856ull / 1000ull * 20ull)
/* The frame of the last request answered: the PC editor asks for /status
 * several times a second while its page is open. */
static unsigned sLastRequestFrame;
static bool sRequestSeen;
enum { PC_EDITOR_GONE_FRAMES = 60 * 3 };
/* The build just written, shown for a while on the bottom screen. */
static char sUploadedName[64];
static unsigned sUploadedUntil;

/* Test mode, only while the link is on: every item and skill, hearts kept
 * full, optionally through walls; the save as it was is kept aside and comes
 * back when the mode ends, and nothing is written to the card meanwhile. */
static bool sTestMode;
static SaveFile* sTestBackup;
extern void Port_DebugAction_GiveAllItems(void);
extern void Port_DebugAction_SetNoclip(int on);
extern int Port_DebugQuery_Noclip(void);
extern void Port_DebugAction_SetAutoNoclip(int on);
extern int Port_Debug_NoclipEnabled(void);
extern void UpdatePlayerSkills(void);
extern void LoadItemGfx(void);
extern void EraseHearts(void);

static bool InGame(void);

/* What the link does goes to tmc3ds.log, so a stall can be traced to it. */
extern void Platform3DS_Debug(const char* line);
static void LinkLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void LinkLog(const char* format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    Platform3DS_Debug(line);
}

bool PortStereoLink_SavesBlocked(void) {
    return sTestMode;
}

static void SetTestMode(bool on) {
    if (on == sTestMode) {
        return;
    }
    LinkLog("[link] test mode %s", on ? "on" : "off");
    if (on) {
        sTestBackup = malloc(sizeof(gSave));
        if (sTestBackup == NULL) {
            return;
        }
        memcpy(sTestBackup, &gSave, sizeof(gSave));
        sTestMode = true;
        Port_DebugAction_GiveAllItems();
        /* The whole world map, as if every area had been visited. */
        memset(gSave.areaVisitFlags, 0xff, sizeof(gSave.areaVisitFlags));
    } else {
        sTestMode = false;
        Port_DebugAction_SetNoclip(0);
        if (sTestBackup != NULL) {
            /* The HUD only ever grows its hearts and erases as many rows as the
             * new count needs; wipe both rows while it still has twenty. */
            gHUD.unk_2 = 1;
            EraseHearts();
            gScreen.bg0.updated = 1;
            memcpy(&gSave, sTestBackup, sizeof(gSave));
            free(sTestBackup);
            sTestBackup = NULL;
            if (InGame()) {
                UpdatePlayerSkills();
                LoadItemGfx();
            }
        }
    }
}

/* A warp asked for before the game is running: press START on the title,
 * continue the last save (or a new game) on file select, then warp. */
static struct {
    bool active;
    int area, room, x, y, layer;
    unsigned frames, fileSelectFrames, settledFrames;
} sPendingGoto;

/* Cells the editor points at: area << 8 | room, and runs of a row. */
static int sHighlightRoom = -1;
static struct {
    u8 row, col0, col1;
} sHighlight[MAX_HIGHLIGHT];
static int sHighlightCount;
static u8 sHighlightMask[SIDE * SIDE];

void PortStereoLink_SetEnabled(bool enabled) {
    sEnabled = enabled;
}

bool PortStereoLink_Enabled(void) {
    return sEnabled;
}

void PortStereoLink_Label(char* out, size_t size) {
    if (!sEnabled) {
        snprintf(out, size, "OFF");
    } else if (sNetFailed) {
        snprintf(out, size, "NO WI-FI");
    } else if (!sNetUp) {
        snprintf(out, size, "WAIT");
    } else {
        const uint32_t a = sAddress;
        const int n = snprintf(out, size, "%u.%u.%u.%u", (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                               (unsigned)((a >> 16) & 0xff), (unsigned)(a >> 24));
        /* The whole address, port too: typed as it is into a browser it
         * opens the editor page. */
        if (n > 0 && (size_t)n < size) {
            snprintf(out + n, size - (size_t)n, ":%d", sPort);
        }
    }
}

static void SetNonBlocking(int socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

static void CloseClient(Client* client) {
    if (client->upload != NULL) {
        char temp[104];
        snprintf(temp, sizeof(temp), "%s.part", client->uploadPath);
        fclose(client->upload);
        remove(temp);
    }
    if (client->socket >= 0) {
        close(client->socket);
    }
    free(client->request);
    free(client->response);
    *client = (Client){ .socket = -1 };
}

static void LinkStop(void) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        CloseClient(&sClients[i]);
    }
    if (sListen >= 0) {
        close(sListen);
        sListen = -1;
    }
    if (sNetUp) {
        PortStereoLink_NetDown();
        sNetUp = false;
    }
    sHighlightRoom = -1;
}

static bool LinkStart(void) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        sClients[i] = (Client){ .socket = -1 };
    }
    if (!PortStereoLink_NetUp(&sAddress)) {
        sNetFailed = true;
        return false;
    }
    sNetUp = true;
    sListen = socket(AF_INET, SOCK_STREAM, 0);
    if (sListen < 0) {
        LinkStop();
        sNetFailed = true;
        return false;
    }
    const int yes = 1;
    setsockopt(sListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    /* A build that left without closing its socket can keep the port taken
     * until the console restarts: then take the next one (the label and the
     * PC editor know). */
    bool bound = false;
    for (sPort = PORT_STEREO_LINK_PORT; sPort < PORT_STEREO_LINK_PORT + 4 && !bound; ++sPort) {
        struct sockaddr_in address = { 0 };
        address.sin_family = AF_INET;
        address.sin_port = htons((u16)sPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        bound = bind(sListen, (struct sockaddr*)&address, sizeof(address)) == 0;
    }
    --sPort;
    if (!bound || listen(sListen, 16) != 0) {
        LinkStop();
        sNetFailed = true;
        return false;
    }
    SetNonBlocking(sListen);
    LinkLog("[link] listening on port %d", sPort);
    return true;
}

/* ---- Background graphics as seen ----
 * Some rooms keep only the tiles near the camera in VRAM and load others as
 * it moves (Festival Town's stalls): VRAM alone draws the far cells wrong.
 * Every cell's 8x8 pixels are kept as they were when last on screen. */
enum { CHAR_BYTES = 32 };
static u8* sRoomChars;     /* [layer][row * cols + col][32], the room's size */
static u8* sRoomCharsHave; /* [layer][row * cols + col] */
static int sRoomCharsRoom = -1, sRoomCharsCols, sRoomCharsRows;

static void CaptureVisibleChars(void) {
    const int room = (gRoomControls.area << 8) | gRoomControls.room;
    if (!InGame() || gRoomControls.scrollAction > 1) {
        return;
    }
    int cols = gRoomControls.width / 8, rows = gRoomControls.height / 8;
    cols = cols < SIDE ? cols : SIDE;
    rows = rows < SIDE ? rows : SIDE;
    if (sRoomCharsRoom != room || sRoomChars == NULL) {
        free(sRoomChars);
        free(sRoomCharsHave);
        const size_t cells = (size_t)cols * (size_t)rows;
        sRoomChars = malloc(2u * cells * CHAR_BYTES);
        sRoomCharsHave = calloc(2u, cells);
        if (sRoomChars == NULL || sRoomCharsHave == NULL) {
            free(sRoomChars);
            free(sRoomCharsHave);
            sRoomChars = sRoomCharsHave = NULL;
            sRoomCharsRoom = -1;
            return;
        }
        sRoomCharsRoom = room;
        sRoomCharsCols = cols;
        sRoomCharsRows = rows;
    }
    const int x0 = (int)gRoomControls.scroll_x - (int)gRoomControls.origin_x;
    const int y0 = (int)gRoomControls.scroll_y - (int)gRoomControls.origin_y;
    for (int layer = 0; layer < 2; ++layer) {
        const MapLayer* map = layer ? &gMapTop : &gMapBottom;
        if (map->bgSettings == NULL) {
            continue;
        }
        const u32 base = ((map->bgSettings->control >> 2) & 3) * 0x4000u;
        for (int row = y0 >> 3; row <= (y0 + 159) >> 3; ++row) {
            for (int col = x0 >> 3; col <= (x0 + 239) >> 3; ++col) {
                if (row < 0 || col < 0 || row >= rows || col >= cols) {
                    continue;
                }
                const u32 tile = (u32)(col >> 1) | ((u32)(row >> 1) << 6);
                const u32 index = Port_Stereo_TileDrawIndex(layer, tile);
                const u16 entry = map->subTiles[index * 4 + ((col & 1) | ((row & 1) << 1))];
                const u32 at = (u32)layer * (u32)(cols * rows) + (u32)row * (u32)cols + (u32)col;
                memcpy(sRoomChars + at * CHAR_BYTES, gVram + ((base + (entry & 0x3ffu) * CHAR_BYTES) & 0xffffu),
                       CHAR_BYTES);
                sRoomCharsHave[at] = 1;
            }
        }
    }
}

/* ---- A camera tour: sprites of the whole room ----
 * The OAM only holds what is on screen. POST /sweep walks the camera over
 * the room -- Link stays where he is: the camera follows a stand-in target --
 * and keeps the sprites seen at each stop; /room then sends them all. */
/* The tilemap streams in a row or column of tiles per 16 pixels of camera
 * travel; no faster than that. */
enum { MAX_SWEEP = 40, SWEEP_SETTLE = 8, SWEEP_GIVE_UP = 180, SWEEP_SPEED = 16 };
typedef struct {
    s16 scrollX, scrollY;
    u16 dispcnt;
    u16 oam[0x200];
    u8 objVram[0x8000];
} SweepShot;
static struct {
    bool active;
    int room; /* area << 8 | room of the shots */
    int count, next, waited;
    s16 targets[MAX_SWEEP][2];
    Entity* savedTarget;
    u8 savedSpeed;
    SweepShot* shots[MAX_SWEEP];
    int shotCount;
} sSweep;
static Entity sSweepTarget;

static void SweepFree(void) {
    for (int i = 0; i < sSweep.shotCount; ++i) {
        free(sSweep.shots[i]);
        sSweep.shots[i] = NULL;
    }
    sSweep.shotCount = 0;
}

static void SweepEnd(void) {
    if (sSweep.active) {
        LinkLog("[link] camera tour done: %d of %d stops", sSweep.shotCount, sSweep.count);
        gRoomControls.camera_target = sSweep.savedTarget;
        gRoomControls.scrollSpeed = sSweep.savedSpeed;
        sSweep.active = false;
    }
}

static bool SweepStart(void) {
    if (!InGame() || gRoomControls.scrollAction > 1 || sSweep.active) {
        return false;
    }
    SweepFree();
    sSweep.room = (gRoomControls.area << 8) | gRoomControls.room;
    sSweep.count = 0;
    /* Camera centres a screen less a margin apart, ends included. */
    const int w = gRoomControls.width, h = gRoomControls.height;
    for (int cy = 80;; cy += 140) {
        const int y = cy + 80 > h ? h - 80 : cy;
        for (int cx = 120;; cx += 210) {
            const int x = cx + 120 > w ? w - 120 : cx;
            if (sSweep.count < MAX_SWEEP) {
                sSweep.targets[sSweep.count][0] = (s16)(gRoomControls.origin_x + (x < 120 ? 120 : x));
                sSweep.targets[sSweep.count][1] = (s16)(gRoomControls.origin_y + (y < 80 ? 80 : y));
                ++sSweep.count;
            }
            if (cx + 120 >= w) {
                break;
            }
        }
        if (cy + 80 >= h) {
            break;
        }
    }
    sSweep.next = 0;
    sSweep.waited = 0;
    sSweep.savedTarget = gRoomControls.camera_target;
    sSweep.savedSpeed = gRoomControls.scrollSpeed;
    gRoomControls.scrollSpeed = SWEEP_SPEED;
    sSweep.active = true;
    LinkLog("[link] camera tour of %02x:%02x, %d stops", gRoomControls.area, gRoomControls.room, sSweep.count);
    return true;
}

static void SweepTick(void) {
    if (!sSweep.active) {
        return;
    }
    if (!InGame() || ((gRoomControls.area << 8) | gRoomControls.room) != sSweep.room) {
        SweepEnd();
        return;
    }
    sSweepTarget.x.HALF.HI = sSweep.targets[sSweep.next][0];
    sSweepTarget.y.HALF.HI = sSweep.targets[sSweep.next][1];
    gRoomControls.camera_target = &sSweepTarget;
    gRoomControls.scrollSpeed = SWEEP_SPEED;
    const int wantX = sSweep.targets[sSweep.next][0] - 120, wantY = sSweep.targets[sSweep.next][1] - 80;
    const bool there = (gRoomControls.scroll_x == wantX || gRoomControls.scroll_x == gRoomControls.origin_x ||
                        gRoomControls.scroll_x == gRoomControls.origin_x + gRoomControls.width - 240) &&
                       (gRoomControls.scroll_y == wantY || gRoomControls.scroll_y == gRoomControls.origin_y ||
                        gRoomControls.scroll_y == gRoomControls.origin_y + gRoomControls.height - 160);
    ++sSweep.waited;
    if (!(there && sSweep.waited >= SWEEP_SETTLE) && sSweep.waited < SWEEP_GIVE_UP) {
        return;
    }
    CaptureVisibleChars();
    SweepShot* shot = malloc(sizeof(SweepShot));
    if (shot != NULL && sSweep.shotCount < MAX_SWEEP) {
        shot->scrollX = gRoomControls.scroll_x;
        shot->scrollY = gRoomControls.scroll_y;
        shot->dispcnt = (u16)(gIoMem[0] | (gIoMem[1] << 8));
        CopyOamWithoutLink((u16*)shot->oam);
        memcpy(shot->objVram, gVram + 0x10000, sizeof(shot->objVram));
        sSweep.shots[sSweep.shotCount++] = shot;
    } else {
        free(shot);
    }
    sSweep.waited = 0;
    if (++sSweep.next >= sSweep.count) {
        SweepEnd();
    }
}

/* ---- Answers ---- */

typedef struct {
    char* data;
    size_t length, capacity;
} Buffer;

/* Every buffer starts with room for the HTTP head, so an answer is sent from
 * it as it is: on the console a second copy of a large answer is memory the
 * game misses (a room after a camera tour is over a megabyte). */
enum { HEAD_RESERVE = 400 };

static bool BufferEnsure(Buffer* b, size_t capacity) {
    if (b->capacity >= capacity) {
        return true;
    }
    char* grown = realloc(b->data, capacity);
    if (grown == NULL) {
        return false;
    }
    if (b->data == NULL) {
        b->length = HEAD_RESERVE;
    }
    b->data = grown;
    b->capacity = capacity;
    return true;
}

static void Put(Buffer* b, const void* data, size_t length) {
    if (b->data == NULL && !BufferEnsure(b, HEAD_RESERVE + 4096)) {
        return;
    }
    if (b->length + length > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < b->length + length) {
            capacity *= 2;
        }
        char* grown = realloc(b->data, capacity);
        if (grown == NULL) {
            return;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
}

static void PutF(Buffer* b, const char* format, ...) __attribute__((format(printf, 2, 3)));
/* Any length: /status outgrew a fixed 512 and went out cut, as JSON no
 * browser would parse. */
static void PutF(Buffer* b, const char* format, ...) {
    char text[512];
    va_list args, again;
    va_start(args, format);
    va_copy(again, args);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n > 0 && n < (int)sizeof(text)) {
        Put(b, text, (size_t)n);
    } else if (n > 0) {
        char* big = malloc((size_t)n + 1);
        if (big != NULL) {
            vsnprintf(big, (size_t)n + 1, format, again);
            Put(b, big, (size_t)n);
            free(big);
        }
    }
    va_end(again);
}

static void PutU16(Buffer* b, unsigned value) {
    const u8 bytes[2] = { (u8)value, (u8)(value >> 8) };
    Put(b, bytes, 2);
}

static int ResponseHead(char* head, size_t size, int status, const char* type, size_t length) {
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 204 ? "No Content"
                       : status == 404 ? "Not Found"
                       : status == 409 ? "Conflict" : "Bad Request";
    return snprintf(head, size,
                    "HTTP/1.0 %d %s\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Access-Control-Allow-Private-Network: true\r\n"
                    "Cache-Control: no-store\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n",
                    status, reason, type, (unsigned)length);
}

static void Respond(Client* client, int status, const char* type, const void* body, size_t length) {
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 204 ? "No Content"
                       : status == 404 ? "Not Found"
                       : status == 409 ? "Conflict" : "Bad Request";
    char head[384];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.0 %d %s\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n"
                           "Access-Control-Allow-Private-Network: true\r\n"
                           "Cache-Control: no-store\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: close\r\n\r\n",
                           status, reason, type, (unsigned)length);
    client->response = malloc((size_t)n + length);
    if (client->response == NULL) {
        CloseClient(client);
        return;
    }
    memcpy(client->response, head, (size_t)n);
    if (length) {
        memcpy(client->response + n, body, length);
    }
    client->responseLength = (size_t)n + length;
    client->responseSent = 0;
}

static void RespondText(Client* client, int status, const char* text) {
    Respond(client, status, "text/plain; charset=utf-8", text, strlen(text));
}

static void RespondBuffer(Client* client, const char* type, Buffer* b) {
    if (b->data == NULL) {
        Respond(client, 200, type, NULL, 0);
        return;
    }
    char head[HEAD_RESERVE];
    const int n = ResponseHead(head, sizeof(head), 200, type, b->length - HEAD_RESERVE);
    /* The head goes right before the body, in the room left for it. */
    memcpy(b->data + HEAD_RESERVE - n, head, (size_t)n);
    client->response = b->data;
    client->responseSent = (size_t)(HEAD_RESERVE - n);
    client->responseLength = b->length;
    b->data = NULL;
}

static int QueryInt(const char* query, const char* name, int fallback) {
    const size_t n = strlen(name);
    for (const char* p = query; p && *p;) {
        if (strncmp(p, name, n) == 0 && p[n] == '=') {
            return (int)strtol(p + n + 1, NULL, 0);
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return fallback;
}

static bool InGame(void) {
    return gMain.task == TASK_GAME && gMapBottom.bgSettings != NULL;
}

static unsigned LayerBgcnt(const MapLayer* layer) {
    return layer->bgSettings != NULL ? layer->bgSettings->control : 0u;
}

static const RoomHeader* RoomHeaderOf(int area, int room) {
    if (area < 0 || area >= LINK_AREA_COUNT || room < 0 || room >= MAX_ROOMS) {
        return NULL;
    }
    const RoomHeader* table = gAreaRoomHeaders[area];
    if (!Port_IsAreaTablePtrReadable((u32)area, table)) {
        Port_RefreshAreaData((u32)area);
        table = gAreaRoomHeaders[area];
        if (!Port_IsAreaTablePtrReadable((u32)area, table)) {
            return NULL;
        }
    }
    for (int i = 0; i <= room; ++i) {
        if (table[i].map_x == 0xffff) {
            return NULL;
        }
    }
    return &table[room];
}

/* What malloc can still hand out: the heap not yet taken from the system,
 * and the free pieces of what was (3DS: libctru's __ctru_heap_size). */
extern unsigned PortStereoLink_LinearFree(void);

/* OAM as drawn, less Link: the PC editor draws him apart, where he is now,
 * so the room's picture must not keep him where he stood when it was taken. */
static void CopyOamWithoutLink(u16* out) {
    memcpy(out, gOamMem, 0x400);
    for (int i = 0; i < MODE1_GBA_OAM_COUNT; ++i) {
        if (virtuappu_mode1_obj_player[i]) {
            out[i * 4] = 0x0200; /* not affine, "double size": hidden */
        }
    }
}

/* Changes whenever Link's own sprites do (pose, place, camera, palette);
 * with `others`, whenever the rest of the room's sprites do. */
static unsigned SpriteRevision(bool others) {
    const u16* oam = (const u16*)gOamMem;
    uint32_t hash = 2166136261u;
    for (int i = 0; i < MODE1_GBA_OAM_COUNT; ++i) {
        const bool hud = ((oam[i * 4 + 2] >> 10) & 3) == 0;
        if (others ? !virtuappu_mode1_obj_player[i] && !hud : virtuappu_mode1_obj_player[i]) {
            for (int k = 0; k < 3; ++k) {
                hash = (hash ^ (uint32_t)(oam[i * 4 + k] + i)) * 16777619u;
            }
        }
    }
    hash = (hash ^ (uint32_t)(u16)gRoomControls.scroll_x) * 16777619u;
    hash = (hash ^ (uint32_t)(u16)gRoomControls.scroll_y) * 16777619u;
    return (unsigned)(hash & 0x7fffffffu);
}

/* Changes when the room's map does: a rock broken, a bush cut, a door open. */
static unsigned MapRevision(void) {
    uint32_t hash = 2166136261u;
    const int cols = gRoomControls.width / 16, rows = gRoomControls.height / 16;
    for (int row = 0; row < rows && row < 64; ++row) {
        for (int col = 0; col < cols && col < 64; ++col) {
            const int i = row * 64 + col;
            hash = (hash ^ gMapBottom.mapData[i]) * 16777619u;
            hash = (hash ^ gMapTop.mapData[i]) * 16777619u;
        }
    }
    return (unsigned)(hash & 0x7fffffffu);
}

/* GET /player[?all=1]: Link's sprites as the console draws them now, or with
 * all=1 every sprite but the HUD's --
 *   "TMCP", u16 DISPCNT, s16 scrollX, s16 scrollY (absolute), u16 entries,
 *   u16 tiles; entries x (u16 attr0, attr1, attr2, u16 flags: bit 0 Link's);
 *   tiles x (u16 slot, 32 bytes of object VRAM at slot * 32); then the
 *   object palette (0x200). */
static bool sPlayerEverything;

static bool PlayerWanted(const u16* oam, int i, bool all) {
    if ((oam[i * 4] >> 14) == 3) {
        return false;
    }
    return all ? (sPlayerEverything || ((oam[i * 4 + 2] >> 10) & 3) != 0) : virtuappu_mode1_obj_player[i] != 0;
}

static void AnswerPlayer(Client* client, bool all) {
    const u16* oam = (const u16*)gOamMem;
    const u16 dispcnt = (u16)(gIoMem[0] | (gIoMem[1] << 8));
    static const u8 kSizes[3][4][2] = { { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
                                         { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
                                         { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } } };
    static u8 used[1024];
    memset(used, 0, sizeof(used));
    int entries = 0, tiles = 0;
    for (int i = 0; i < MODE1_GBA_OAM_COUNT; ++i) {
        if (!PlayerWanted(oam, i, all)) {
            continue;
        }
        const u16 a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        const int shape = a0 >> 14;
        if (!(a0 & 0x100) && (a0 & 0x200)) {
            continue; /* hidden */
        }
        ++entries;
        const int w = kSizes[shape][a1 >> 14][0], h = kSizes[shape][a1 >> 14][1];
        const int step = (a0 & 0x2000) ? 2 : 1, oneD = (dispcnt & 0x40) != 0;
        for (int ty = 0; ty < h / 8; ++ty) {
            for (int tx = 0; tx < w / 8; ++tx) {
                const int t = (a2 & 0x3ff) + (oneD ? (ty * (w / 8) + tx) * step : ty * 32 + tx * step);
                for (int k = 0; k < step; ++k) {
                    const int slot = (t + k) & 0x3ff;
                    if (!used[slot]) {
                        used[slot] = 1;
                        ++tiles;
                    }
                }
            }
        }
    }
    Buffer b = { 0 };
    Put(&b, "TMCP", 4);
    PutU16(&b, dispcnt);
    PutU16(&b, (unsigned)(u16)gRoomControls.scroll_x);
    PutU16(&b, (unsigned)(u16)gRoomControls.scroll_y);
    PutU16(&b, (unsigned)entries);
    PutU16(&b, (unsigned)tiles);
    for (int i = 0; i < MODE1_GBA_OAM_COUNT; ++i) {
        if (PlayerWanted(oam, i, all) && !(!(oam[i * 4] & 0x100) && (oam[i * 4] & 0x200))) {
            Put(&b, &oam[i * 4], 6);
            /* bit 0 Link's; bits 8-15 the depth tag it was drawn with */
            PutU16(&b, (virtuappu_mode1_obj_player[i] ? 1u : 0u) | ((unsigned)virtuappu_mode1_obj_stereo_depth[i] << 8));
        }
    }
    for (int slot = 0; slot < 1024; ++slot) {
        if (used[slot]) {
            PutU16(&b, (unsigned)slot);
            Put(&b, gVram + 0x10000 + slot * 32, 32);
        }
    }
    Put(&b, gPaletteBuffer + 256, 0x200);
    RespondBuffer(client, "application/octet-stream", &b);
}
extern void PlatformGpu3DS_EditorCellsInfo(char* out, size_t size);

static unsigned HeapLeft(void) {
#ifdef TMC_3DS
    extern u32 __ctru_heap_size;
    const struct mallinfo info = mallinfo();
    return (unsigned)(__ctru_heap_size - (u32)info.arena + (u32)info.fordblks);
#else
    return 0;
#endif
}

static void AnswerStatus(Client* client) {
    Buffer b = { 0 };
    /* The backgrounds shown: number, priority, editor depth (or null). */
    char bgs[192] = "";
    {
        const u16 dispcnt = (u16)(gIoMem[0] | (gIoMem[1] << 8));
        size_t n = 0;
        for (int bg = 0; bg < 4; ++bg) {
            if (!(dispcnt & (0x100 << bg))) {
                continue;
            }
            const u16 bgcnt = (u16)(gIoMem[8 + bg * 2] | (gIoMem[9 + bg * 2] << 8));
            int depth;
            const bool set = PortStereoEdits_ScreenDepth(Port_Stereo_ScreenKey(), bg, &depth);
            char one[48];
            if (set) {
                snprintf(one, sizeof(one), "%s{\"bg\":%d,\"prio\":%u,\"depth\":%d}", n ? "," : "", bg, bgcnt & 3, depth);
            } else {
                snprintf(one, sizeof(one), "%s{\"bg\":%d,\"prio\":%u,\"depth\":null}", n ? "," : "", bg, bgcnt & 3);
            }
            n += (size_t)snprintf(bgs + n, sizeof(bgs) - n, "%s", one);
        }
    }
    const RoomHeader* header = RoomHeaderOf(gRoomControls.area, gRoomControls.room);
    char cellsInfo[128];
    PlatformGpu3DS_EditorCellsInfo(cellsInfo, sizeof(cellsInfo));
    PutF(&b,
         "{\"inGame\":%s,\"live\":%s,\"area\":%u,\"room\":%u,\"width\":%u,\"height\":%u,"
         "\"originX\":%u,\"originY\":%u,\"scrollX\":%d,\"scrollY\":%d,\"linkX\":%d,\"linkY\":%d,"
         "\"tileset\":%u,\"transition\":%s,\"rev\":%lu,\"frame\":%u,\"selRev\":%u,\"editor\":%s,"
         "\"fade\":%s,\"starting\":%s,\"task\":%u,\"test\":%s,\"noclip\":%s,\"health\":%u,\"maxHealth\":%u,"
         "\"hudMax\":%u,\"sweep\":%s,\"sweepDone\":%d,\"sweepRoom\":%d,\"heapFree\":%u,"
         "\"inRoom\":%s,\"screen\":\"%08lx\",\"bgs\":[%s],\"linearFree\":%u,\"editorCells\":%s,"
         "\"editorSel\":%d,\"playerRev\":%u,\"spriteRev\":%u,\"mapRev\":%u,\"viewW\":%d,\"viewH\":%d,\"walls\":%s,\"heightsRev\":%u,\"frameShown\":%s}",
         InGame() ? "true" : "false", Port_Stereo_ReliefLive() ? "true" : "false", gRoomControls.area,
         gRoomControls.room, gRoomControls.width, gRoomControls.height, gRoomControls.origin_x,
         gRoomControls.origin_y, gRoomControls.scroll_x, gRoomControls.scroll_y,
         (int)gPlayerEntity.base.x.HALF.HI - (int)gRoomControls.origin_x,
         (int)gPlayerEntity.base.y.HALF.HI - (int)gRoomControls.origin_y, header ? header->tileSet_id : 0u,
         gRoomControls.scrollAction > 1 ? "true" : "false", (unsigned long)PortStereoEdits_Revision(), sFrame,
         PortStereoEditor_SelectionRevision(), PortStereoEditor_IsOpen() ? "true" : "false",
         gFadeControl.active ? "true" : "false", sPendingGoto.active ? "true" : "false", gMain.task,
         sTestMode ? "true" : "false", sNoclipWanted ? "true" : "false", gSave.stats.health,
         gSave.stats.maxHealth, gHUD.maxHealth, sSweep.active ? "true" : "false", sSweep.shotCount, sSweep.room,
         HeapLeft(), Port_Stereo_InRoom() ? "true" : "false", (unsigned long)Port_Stereo_ScreenKey(), bgs, PortStereoLink_LinearFree(), cellsInfo,
         PortStereoEditor_SelectedCount(), SpriteRevision(false), SpriteRevision(true), MapRevision(),
         Port_Widescreen_GameplayViewWidth(), Port_Widescreen_GameplayViewHeight(),
         Port_Debug_NoclipEnabled() ? "false" : "true", HeightsRevision(),
         sHighlightHidden ? "false" : "true");
    RespondBuffer(client, "application/json", &b);
}

static void AnswerRooms(Client* client) {
    Buffer b = { 0 };
    Put(&b, "[", 1);
    bool first = true;
    for (int area = 0; area < LINK_AREA_COUNT; ++area) {
        for (int room = 0; room < MAX_ROOMS; ++room) {
            const RoomHeader* h = RoomHeaderOf(area, room);
            if (h == NULL) {
                break;
            }
            if (h->pixel_width == 0 || h->pixel_height == 0) {
                continue;
            }
            PutF(&b, "%s[%d,%d,%u,%u,%u,%u,%u]", first ? "" : ",", area, room, h->map_x, h->map_y, h->pixel_width,
                 h->pixel_height, h->tileSet_id);
            first = false;
        }
    }
    Put(&b, "]", 1);
    RespondBuffer(client, "application/json", &b);
}

typedef struct {
    Buffer* b;
    int count;
    bool json;
} EntityOut;

static void VisitEntities(EntityOut* out) {
    for (int l = 0; l < 9; ++l) {
        LinkedList* list = &gEntityLists[l];
        if (list->first == NULL) {
            continue;
        }
        int guard = 0;
        for (Entity* e = list->first; e != NULL && e != (Entity*)list && guard < 512; e = e->next, ++guard) {
            if (e->kind == MANAGER || (e->flags & ENT_DELETED)) {
                continue;
            }
            const int x = (int)e->x.HALF.HI - (int)gRoomControls.origin_x;
            const int y = (int)e->y.HALF.HI - (int)gRoomControls.origin_y;
            const int z = e->z.HALF.HI;
            const int depth = PortStereoEdits_EntityDelta(gRoomControls.area, gRoomControls.room, e->kind, e->id,
                                                          e->type, x >> 3, y >> 3);
            if (out->json) {
                PutF(out->b, "%s{\"kind\":%u,\"id\":%u,\"type\":%u,\"type2\":%u,\"x\":%d,\"y\":%d,\"z\":%d,"
                             "\"drawn\":%s,\"depth\":%d}",
                     out->count ? "," : "", e->kind, e->id, e->type, e->type2, x, y, z,
                     e->spriteSettings.draw ? "true" : "false", depth);
            } else if (out->b != NULL) {
                const u8 head[4] = { e->kind, e->id, e->type, e->type2 };
                Put(out->b, head, 4);
                PutU16(out->b, (unsigned)(s16)x);
                PutU16(out->b, (unsigned)(s16)y);
                PutU16(out->b, (unsigned)(s16)z);
                const u8 tail[2] = { (u8)(s8)depth, 0 };
                Put(out->b, tail, 2);
            }
            ++out->count;
        }
    }
}

static void AnswerEntities(Client* client) {
    Buffer b = { 0 };
    Put(&b, "[", 1);
    EntityOut out = { &b, 0, true };
    if (InGame()) {
        VisitEntities(&out);
    }
    Put(&b, "]", 1);
    RespondBuffer(client, "application/json", &b);
}

static void PutLayer(Buffer* b, const MapLayer* layer) {
    Put(b, layer->mapData, sizeof(layer->mapData));
    Put(b, layer->collisionData, sizeof(layer->collisionData));
    Put(b, layer->actTiles, sizeof(layer->actTiles));
    Put(b, layer->subTiles, sizeof(layer->subTiles));
}

/* Changes when the relief's own heights of the room do (a pit opening, a
 * door, a layer the game swaps): the PC editor then takes them again. */
static unsigned HeightsRevision(void) {
    PortStereoRoomView view;
    if (!InGame() || !Port_Stereo_RoomView(&view)) {
        return 0;
    }
    uint32_t hash = 2166136261u;
    const size_t n = (size_t)view.cols * (size_t)view.rows;
    const s8* grids[3] = { view.autoGround, view.autoHeight, view.autoHeightTop };
    for (int g = 0; g < 3; ++g) {
        for (size_t i = 0; i < n; ++i) {
            hash = (hash ^ (uint32_t)(u8)grids[g][i]) * 16777619u;
        }
    }
    return (unsigned)(hash & 0x7fffffffu);
}

/* GET /heights: "TMCH", u16 cols, u16 rows, then autoGround, autoHeight,
 * autoHeightTop as SIDE*SIDE grids, as the relief has them now. */
static void PutGrid(Buffer* b, const void* grid, const PortStereoRoomView* view);
static void AnswerHeights(Client* client) {
    PortStereoRoomView view;
    if (!InGame() || !Port_Stereo_RoomView(&view)) {
        RespondText(client, 409, "not in a room");
        return;
    }
    Buffer b = { 0 };
    Put(&b, "TMCH", 4);
    PutU16(&b, (unsigned)view.cols);
    PutU16(&b, (unsigned)view.rows);
    PutGrid(&b, view.autoGround, &view);
    PutGrid(&b, view.autoHeight, &view);
    PutGrid(&b, view.autoHeightTop, &view);
    RespondBuffer(client, "application/octet-stream", &b);
}

static void PutGrid(Buffer* b, const void* grid, const PortStereoRoomView* view) {
    static u8 block[SIDE * SIDE];
    memset(block, 0, sizeof(block));
    for (int row = 0; row < view->rows && row < SIDE; ++row) {
        memcpy(&block[row * SIDE], (const u8*)grid + row * view->cols, (size_t)(view->cols < SIDE ? view->cols : SIDE));
    }
    Put(b, block, sizeof(block));
}

static void AnswerRoom(Client* client) {
    PortStereoRoomView view;
    if (!InGame() || gRoomControls.scrollAction > 1 || !Port_Stereo_RoomView(&view)) {
        RespondText(client, 409, "not in a room");
        return;
    }
    const RoomHeader* header = RoomHeaderOf(gRoomControls.area, gRoomControls.room);
    EntityOut count = { NULL, 0, false };
    VisitEntities(&count);
    Buffer b = { 0 };
    /* One allocation of the right size, not a doubling one. */
    {
        const size_t cells = (size_t)view.cols * (size_t)view.rows;
        const size_t shots = sSweep.room == ((gRoomControls.area << 8) | gRoomControls.room) ? (size_t)sSweep.shotCount : 0;
        const size_t size = HEAD_RESERVE + 32 + 2 * (8192 + 4096 + 4096 + 16384) + 0x10000 + 0x200 + 4 * 16384 +
                            2 * 16384 + (size_t)count.count * 12 + (8 + 0x400 + 0x8000 + 0x200) +
                            shots * (12 + 0x400 + 0x8000) + (8 + 2 * cells * (1 + CHAR_BYTES)) + 4 + 16384 + 64;
        if (!BufferEnsure(&b, size)) {
            RespondText(client, 409, "not enough memory for the room");
            LinkLog("[link] no memory for a %u-byte room", (unsigned)size);
            return;
        }
    }
    Put(&b, "TMCR", 4);
    PutU16(&b, 1);
    const u8 ids[2] = { gRoomControls.area, gRoomControls.room };
    Put(&b, ids, 2);
    PutU16(&b, gRoomControls.width);
    PutU16(&b, gRoomControls.height);
    PutU16(&b, gRoomControls.origin_x);
    PutU16(&b, gRoomControls.origin_y);
    PutU16(&b, LayerBgcnt(&gMapBottom));
    PutU16(&b, LayerBgcnt(&gMapTop));
    PutU16(&b, header ? header->tileSet_id : 0u);
    PutU16(&b, (unsigned)view.cols);
    PutU16(&b, (unsigned)view.rows);
    PutU16(&b, (unsigned)gRoomControls.scroll_x);
    PutU16(&b, (unsigned)gRoomControls.scroll_y);
    PutU16(&b, (unsigned)count.count);
    PutLayer(&b, &gMapBottom);
    PutLayer(&b, &gMapTop);
    Put(&b, gVram, 0x10000);
    /* The game's own palette, not the one in hardware, which a fade changes. */
    Put(&b, gPaletteBuffer, 0x200);
    PutGrid(&b, view.autoGround, &view);
    PutGrid(&b, view.autoHeight, &view);
    PutGrid(&b, view.autoHeightTop, &view);
    PutGrid(&b, view.kind, &view);
    Put(&b, view.tileHash[0], sizeof(u32) * 64 * 64);
    Put(&b, view.tileHash[1], sizeof(u32) * 64 * 64);
    EntityOut entities = { &b, 0, false };
    VisitEntities(&entities);
    /* The sprites as drawn this frame: furniture, door frames and the like
     * are objects, not map tiles. */
    Put(&b, "OAMS", 4);
    PutU16(&b, (unsigned)(gIoMem[0] | (gIoMem[1] << 8)));
    PutU16(&b, 0);
    {
        static u16 oam[0x200];
        CopyOamWithoutLink(oam);
        Put(&b, oam, 0x400);
    }
    Put(&b, gVram + 0x10000, 0x8000);
    Put(&b, gPaletteBuffer + 256, 0x200);
    /* The camera tour's sprites, for this room. */
    if (!sSweep.active && sSweep.room == ((gRoomControls.area << 8) | gRoomControls.room)) {
        for (int i = 0; i < sSweep.shotCount; ++i) {
            const SweepShot* shot = sSweep.shots[i];
            Put(&b, "OAMX", 4);
            PutU16(&b, (unsigned)shot->scrollX);
            PutU16(&b, (unsigned)shot->scrollY);
            PutU16(&b, shot->dispcnt);
            PutU16(&b, 0);
            Put(&b, shot->oam, sizeof(shot->oam));
            Put(&b, shot->objVram, sizeof(shot->objVram));
        }
    }
    /* The background graphics of every cell as last seen on screen. */
    CaptureVisibleChars();
    if (sRoomChars != NULL && sRoomCharsRoom == ((gRoomControls.area << 8) | gRoomControls.room)) {
        const size_t cells = (size_t)sRoomCharsCols * (size_t)sRoomCharsRows;
        Put(&b, "CHRS", 4);
        PutU16(&b, (unsigned)sRoomCharsCols);
        PutU16(&b, (unsigned)sRoomCharsRows);
        for (int layer = 0; layer < 2; ++layer) {
            Put(&b, sRoomCharsHave + (size_t)layer * cells, cells);
            Put(&b, sRoomChars + (size_t)layer * cells * CHAR_BYTES, cells * CHAR_BYTES);
        }
    }
    /* What each map tile is drawn with: special tiles (0x4000 and up) are
     * not plain tile set entries. */
    Put(&b, "RIDX", 4);
    for (int layer = 0; layer < 2; ++layer) {
        for (u32 pos = 0; pos < 64 * 64; ++pos) {
            PutU16(&b, Port_Stereo_TileDrawIndex(layer, pos));
        }
    }
    RespondBuffer(client, "application/octet-stream", &b);
}

static void AnswerEdits(Client* client, const char* query, bool post, const char* body, size_t bodyLength) {
    const int kinds = QueryInt(query, "kinds", PORT_STEREO_EXPORT_ALL) & PORT_STEREO_EXPORT_ALL;
    const int area = QueryInt(query, "area", gRoomControls.area);
    const int room = QueryInt(query, "room", gRoomControls.room);
    if (post) {
        PortStereoEdits_Import(kinds, area, room, body, bodyLength);
        char text[48];
        snprintf(text, sizeof(text), "ok %lu", (unsigned long)PortStereoEdits_Revision());
        RespondText(client, 200, text);
        return;
    }
    size_t length = 0;
    char* text = PortStereoEdits_Export(kinds, (kinds & PORT_STEREO_EXPORT_CELLS) ? area : -1, room, &length);
    if (text == NULL) {
        RespondText(client, 400, "out of memory");
        return;
    }
    Respond(client, 200, "text/plain; charset=utf-8", text, length);
    free(text);
}

bool PortStereoLink_RoomSize(int area, int room, int* width, int* height) {
    const RoomHeader* header = RoomHeaderOf(area, room);
    if (header == NULL || header->pixel_width == 0 || header->pixel_height == 0) {
        return false;
    }
    *width = header->pixel_width;
    *height = header->pixel_height;
    return true;
}

int PortStereoLink_Goto(int area, int room, int x, int y, int layer) {
    const RoomHeader* header = RoomHeaderOf(area, room);
    if (header == NULL) {
        return 404;
    }
    if (!InGame() || gRoomControls.scrollAction > 1 || gRoomTransition.transitioningOut) {
        return 409;
    }
    Transition t = { 0 };
    t.warp_type = WARP_TYPE_AREA;
    t.area = (u8)area;
    t.room = (u8)room;
    t.endX = (u16)(x >= 0 ? x : header->pixel_width / 2);
    t.endY = (u16)(y >= 0 ? y : header->pixel_height / 2);
    t.layer = (u8)(layer > 0 ? layer : 1);
    gRoomTransition.stairs_idx = 0;
    DoExitTransition(&t);
    return 200;
}

static void AnswerGoto(Client* client, const char* query) {
    if (gMain.task == TASK_TITLE || gMain.task == TASK_FILE_SELECT) {
        const int area = QueryInt(query, "area", -1), room = QueryInt(query, "room", -1);
        if (RoomHeaderOf(area, room) == NULL) {
            RespondText(client, 404, "no such room");
            return;
        }
        sPendingGoto = (typeof(sPendingGoto)){ true, area, room, QueryInt(query, "x", -1), QueryInt(query, "y", -1),
                                               QueryInt(query, "layer", 1), 0, 0, 0 };
        RespondText(client, 202, "starting the game, then warping");
        return;
    }
    const int status = PortStereoLink_Goto(QueryInt(query, "area", -1), QueryInt(query, "room", -1),
                                           QueryInt(query, "x", -1), QueryInt(query, "y", -1),
                                           QueryInt(query, "layer", 1));
    RespondText(client, status, status == 200 ? "ok" : status == 404 ? "no such room" : "busy");
}

/* POST /file?name=x.3dsx: a new build into sdmc:/3ds/, so a test build
 * reaches the console while the game runs; only a plain .3dsx name. */
static void RespondText(Client* client, int status, const char* text);

/* Starts writing the body of POST /file?name=x.3dsx to sdmc:/3ds/x.3dsx.part;
 * false (answered) when the name is not a plain .3dsx. */
#define LAUNCHER_PATH "sdmc:/boot.3dsx"
#define LAUNCHER_BACKUP "sdmc:/boot.3dsx.orig"

static bool UploadStart(Client* client, const char* query, size_t contentLength, bool launcher) {
    if (launcher) {
        /* A Homebrew Launcher is far smaller than the game, far larger than
         * nothing. */
        if (contentLength < 64 * 1024 || contentLength > 4 * 1024 * 1024) {
            RespondText(client, 400, "that is no Homebrew Launcher");
            return false;
        }
        snprintf(client->uploadPath, sizeof(client->uploadPath), "%s", LAUNCHER_PATH);
        client->uploadKeep = true;
        client->uploadLauncher = true;
        client->upload = fopen(LAUNCHER_PATH ".part", "wb");
        if (client->upload == NULL) {
            RespondText(client, 400, "cannot write");
            return false;
        }
        client->uploadLeft = contentLength;
        client->uploadDone = 0;
        client->uploadFailed = false;
        LinkLog("[link] receiving a launcher, %u bytes", (unsigned)contentLength);
        return true;
    }
    char name[64] = { 0 };
    const char* at = query ? strstr(query, "name=") : NULL;
    if (at != NULL) {
        sscanf(at + 5, "%63[A-Za-z0-9._-]", name);
    }
    const size_t n = strlen(name);
    if (n < 6 || strcmp(name + n - 5, ".3dsx") != 0 || strstr(name, "..") != NULL || contentLength < 4) {
        RespondText(client, 400, "only a .3dsx, by name, into sdmc:/3ds/");
        return false;
    }
    char temp[104];
    client->uploadKeep = strstr(query, "quit=0") != NULL;
    snprintf(client->uploadPath, sizeof(client->uploadPath), "sdmc:/3ds/%s", name);
    snprintf(temp, sizeof(temp), "%s.part", client->uploadPath);
    client->upload = fopen(temp, "wb");
    if (client->upload == NULL) {
        RespondText(client, 400, "cannot write");
        return false;
    }
    client->uploadLeft = contentLength;
    client->uploadDone = 0;
    client->uploadFailed = false;
    LinkLog("[link] receiving %s, %u bytes", client->uploadPath, (unsigned)contentLength);
    return true;
}

static void UploadWrite(Client* client, const char* data, size_t length) {
    if (length > client->uploadLeft) {
        length = client->uploadLeft;
    }
    for (size_t i = 0; client->uploadDone + i < 4 && i < length; ++i) {
        client->uploadMagic[client->uploadDone + i] = data[i];
    }
    if (!client->uploadFailed && fwrite(data, 1, length, client->upload) != length) {
        client->uploadFailed = true;
    }
    client->uploadDone += length;
    client->uploadLeft -= length;
}

static void UploadFinish(Client* client) {
    char temp[104];
    snprintf(temp, sizeof(temp), "%s.part", client->uploadPath);
    const bool closed = fclose(client->upload) == 0;
    client->upload = NULL;
    if (client->uploadFailed || !closed || memcmp(client->uploadMagic, "3DSX", 4) != 0) {
        remove(temp);
        LinkLog("[link] dropped %s", client->uploadPath);
        RespondText(client, 400, "cannot write, or not a 3DSX");
        return;
    }
    if (client->uploadLauncher) {
        /* The launcher the console came with is kept once, never replaced:
         * POST /launcher?restore=1 puts it back. */
        FILE* backup = fopen(LAUNCHER_BACKUP, "rb");
        if (backup != NULL) {
            fclose(backup);
        } else {
            FILE* current = fopen(LAUNCHER_PATH, "rb");
            if (current != NULL) {
                fclose(current);
                if (rename(LAUNCHER_PATH, LAUNCHER_BACKUP) != 0) {
                    remove(temp);
                    LinkLog("[link] launcher: could not keep the old one, nothing changed");
                    RespondText(client, 500, "could not keep the old launcher; nothing changed");
                    return;
                }
                LinkLog("[link] launcher: old one kept as " LAUNCHER_BACKUP);
            }
        }
    }
    remove(client->uploadPath);
    const bool renamed = rename(temp, client->uploadPath) == 0;
    if (client->uploadLauncher) {
        LinkLog("[link] launcher replaced: %s", renamed ? "ok" : "rename failed");
        RespondText(client, renamed ? 200 : 500, renamed ? "ok, launcher replaced" : "cannot rename");
        return;
    }
    LinkLog("[link] wrote %s: %s", client->uploadPath, renamed ? "ok" : "rename failed");
    if (renamed) {
        const char* name = strrchr(client->uploadPath, '/');
        snprintf(sUploadedName, sizeof(sUploadedName), "%s", name ? name + 1 : client->uploadPath);
        sUploadedUntil = sFrame + 60 * 8;
        /* The game quits by itself a little later -- back to the Homebrew
         * Launcher, which then starts the new build when asked. A plain quit;
         * it is only asking hb:ldr to relaunch that hung the console. */
        if (!client->uploadKeep) {
            sQuitIn = QUIT_AFTER_UPLOAD_FRAMES;
            sQuitAt = sFrame + QUIT_AFTER_UPLOAD_FRAMES;
        }
    }
    RespondText(client, renamed ? 200 : 400, renamed ? "ok" : "cannot rename");
}

/* GET /: the PC editor itself, packed into romfs by the build, so a browser
 * (a headset's, say) can open it from the console with nothing on a PC. */
static void AnswerEditorPage(Client* client) {
    static char* page;
    static size_t pageLength;
    if (page == NULL) {
        FILE* file = fopen(PORT_STEREO_LINK_EDITOR_PAGE, "rb");
        if (file != NULL) {
            fseek(file, 0, SEEK_END);
            const long length = ftell(file);
            fseek(file, 0, SEEK_SET);
            page = length > 0 ? malloc((size_t)length) : NULL;
            if (page != NULL && fread(page, 1, (size_t)length, file) == (size_t)length) {
                pageLength = (size_t)length;
            } else {
                free(page);
                page = NULL;
            }
            fclose(file);
        }
    }
    if (page == NULL) {
        RespondText(client, 200, "The Minish Cap 3DS - stereo 3D link. Open the PC editor and connect here.");
        return;
    }
    Respond(client, 200, "text/html; charset=utf-8", page, pageLength);
}

static void AnswerSelect(Client* client, const char* query, const char* body, size_t bodyLength) {
    const int area = QueryInt(query, "area", gRoomControls.area);
    const int room = QueryInt(query, "room", gRoomControls.room);
    sHighlightCount = 0;
    sHighlightRoom = (area << 8) | room;
    memset(sHighlightMask, 0, sizeof(sHighlightMask));
    if (bodyLength == 0) {
        PortStereoEditor_ClearSelection(area, room);
    }
    const char* end = body + bodyLength;
    while (body < end && sHighlightCount < MAX_HIGHLIGHT) {
        char line[48];
        const char* newline = memchr(body, '\n', (size_t)(end - body));
        const size_t n = (size_t)((newline ? newline : end) - body);
        if (n < sizeof(line)) {
            memcpy(line, body, n);
            line[n] = '\0';
            unsigned row, col0, col1;
            if (sscanf(line, "%u %u %u", &row, &col0, &col1) == 3 && row < SIDE && col0 <= col1 && col1 < SIDE) {
                if (sHighlightCount == 0) {
                    PortStereoEditor_ClearSelection(area, room);
                }
                PortStereoEditor_SelectRun(area, room, (int)row, (int)col0, (int)col1);
                memset(&sHighlightMask[row * SIDE + col0], 1, col1 - col0 + 1);
                sHighlight[sHighlightCount].row = (u8)row;
                sHighlight[sHighlightCount].col0 = (u8)col0;
                sHighlight[sHighlightCount].col1 = (u8)col1;
                ++sHighlightCount;
            }
        }
        body += n + 1;
    }
    RespondText(client, 200, "ok");
}

static bool Highlighted(int col, int row) {
    return col >= 0 && row >= 0 && col < SIDE && row < SIDE && sHighlightMask[row * SIDE + col];
}

/* The outline of the cells the PC editor points at, as one-pixel lines in
 * GBA screen coordinates {x, y, w, h}: runs of cell edges joined. */
bool PortStereoLink_HighlightShown(void) {
    return !sHighlightHidden;
}

void PortStereoLink_ShowHighlight(bool shown) {
    sHighlightHidden = !shown;
}

int PortStereoLink_Highlight(float (*rects)[4], int max) {
    if (!sEnabled || sHighlightHidden || sHighlightCount == 0 || sHighlightRoom != ((gRoomControls.area << 8) | gRoomControls.room) ||
        !InGame()) {
        return 0;
    }
    const int x0 = (int)gRoomControls.origin_x - (int)gRoomControls.scroll_x;
    const int y0 = (int)gRoomControls.origin_y - (int)gRoomControls.scroll_y;
    const int col0 = (-x0) >> 3, row0 = (-y0) >> 3, col1 = col0 + 31, row1 = row0 + 21;
    int n = 0;
    /* Top and bottom edges, joined along each row. */
    for (int row = row0; row <= row1 && n < max; ++row) {
        for (int side = 0; side < 2; ++side) {
            const int other = side ? row + 1 : row - 1;
            for (int col = col0; col <= col1 && n < max;) {
                if (!(Highlighted(col, row) && !Highlighted(col, other))) {
                    ++col;
                    continue;
                }
                int end = col;
                while (end + 1 <= col1 && Highlighted(end + 1, row) && !Highlighted(end + 1, other)) {
                    ++end;
                }
                rects[n][0] = (float)(x0 + col * 8);
                rects[n][1] = (float)(y0 + row * 8 + (side ? 7 : 0));
                rects[n][2] = (float)((end - col + 1) * 8);
                rects[n][3] = 1.0f;
                ++n;
                col = end + 1;
            }
        }
    }
    /* Left and right edges, joined down each column. */
    for (int col = col0; col <= col1 && n < max; ++col) {
        for (int side = 0; side < 2; ++side) {
            const int other = side ? col + 1 : col - 1;
            for (int row = row0; row <= row1 && n < max;) {
                if (!(Highlighted(col, row) && !Highlighted(other, row))) {
                    ++row;
                    continue;
                }
                int end = row;
                while (end + 1 <= row1 && Highlighted(col, end + 1) && !Highlighted(other, end + 1)) {
                    ++end;
                }
                rects[n][0] = (float)(x0 + col * 8 + (side ? 7 : 0));
                rects[n][1] = (float)(y0 + row * 8);
                rects[n][2] = 1.0f;
                rects[n][3] = (float)((end - row + 1) * 8);
                ++n;
                row = end + 1;
            }
        }
    }
    return n;
}

static void Answer(Client* client) {
    char* request = client->request;
    char* headerEnd = strstr(request, "\r\n\r\n");
    char method[8] = { 0 }, target[256] = { 0 };
    if (sscanf(request, "%7s %255s", method, target) != 2) {
        RespondText(client, 400, "bad request");
        return;
    }
    char* query = strchr(target, '?');
    if (query) {
        *query++ = '\0';
    }
    const char* body = headerEnd + 4;
    const size_t bodyLength = client->requestLength - (size_t)(body - request);
    const bool post = strcmp(method, "POST") == 0;
    if (strcmp(method, "OPTIONS") == 0) {
        Respond(client, 204, "text/plain", NULL, 0);
    } else if (strcmp(target, "/status") == 0) {
        AnswerStatus(client);
    } else if (strcmp(target, "/rooms") == 0) {
        AnswerRooms(client);
    } else if (strcmp(target, "/room") == 0) {
        AnswerRoom(client);
    } else if (strcmp(target, "/cell") == 0) {
        /* The heights the relief uses, corrections included, for checking. */
        PortStereoRoomView view;
        char text[160];
        const int col = QueryInt(query, "col", 0), row = QueryInt(query, "row", 0);
        if (InGame() && Port_Stereo_RoomView(&view) && col >= 0 && row >= 0 && col < view.cols && row < view.rows) {
            const int at = row * view.cols + col;
            snprintf(text, sizeof(text), "{\"bottom\":%d,\"top\":%d,\"ground\":%d,\"autoBottom\":%d,\"autoTop\":%d,\"kind\":%u}",
                     view.height[at], view.heightTop[at], view.ground[at], view.autoHeight[at], view.autoHeightTop[at],
                     view.kind[at]);
            Respond(client, 200, "application/json", text, strlen(text));
        } else {
            RespondText(client, 404, "no such cell");
        }
    } else if (strcmp(target, "/debug-bg0") == 0) {
        Buffer b = { 0 };
        for (int i = 0x20; i < 0x60; ++i) {
            PutF(&b, "%04x%s", gBG0Buffer[i], (i & 15) == 15 ? "\n" : " ");
        }
        PutF(&b, "unk_2=%u hudMax=%u hudHealth=%u\n", gHUD.unk_2, gHUD.maxHealth, gHUD.health);
        RespondBuffer(client, "text/plain", &b);
    } else if (strcmp(target, "/selection") == 0) {
        /* What the console's own 3D editor has selected: "area room" then
         * "row col0 col1" lines. */
        size_t length = 0;
        char* text = PortStereoEditor_SelectionText(&length);
        if (text != NULL) {
            Respond(client, 200, "text/plain; charset=utf-8", text, length);
            free(text);
        } else {
            RespondText(client, 200, "");
        }
    } else if (strcmp(target, "/bottom") == 0) {
        const uint32_t* pixels;
        unsigned pitch;
        if (PortStereoLink_BottomImage(&pixels, &pitch)) {
            Buffer b = { 0 };
            Put(&b, "TMCB", 4);
            PutU16(&b, 320);
            PutU16(&b, 240);
            for (unsigned y = 0; y < 240; ++y) {
                Put(&b, pixels + (size_t)y * pitch, 320 * sizeof(uint32_t));
            }
            RespondBuffer(client, "application/octet-stream", &b);
        } else {
            RespondText(client, 409, "no bottom image yet");
        }
    } else if (strcmp(target, "/frame") == 0) {
        PortStereoLink_FrameRequest();
        client->frameWait = 1;
    } else if (strcmp(target, "/heights") == 0) {
        AnswerHeights(client);
    } else if (strcmp(target, "/editor") == 0) {
        char text[768], cells[128];
        PortStereoEditor_Geometry(text, sizeof(text));
        PlatformGpu3DS_EditorCellsInfo(cells, sizeof(cells));
        const size_t n = strlen(text);
        if (n > 0 && n + strlen(cells) + 16 < sizeof(text)) {
            snprintf(text + n - 1, sizeof(text) - (n - 1), ",\"cells\":%s}", cells);
        }
        Respond(client, 200, "application/json", text, strlen(text));
    } else if (strcmp(target, "/player") == 0) {
        sPlayerEverything = QueryInt(query, "all", 0) == 2; /* the HUD's too: menus, the title */
        AnswerPlayer(client, QueryInt(query, "all", 0) != 0);
    } else if (strcmp(target, "/entities") == 0) {
        AnswerEntities(client);
    } else if (strcmp(target, "/edits") == 0) {
        AnswerEdits(client, query, post, body, bodyLength);
    } else if (post && strcmp(target, "/goto") == 0) {
        AnswerGoto(client, query);
    } else if (post && strcmp(target, "/press") == 0) {
        static const struct { const char* name; uint32_t key; } kNames[] = {
            { "a", PORT_LINK_KEY_A },         { "b", PORT_LINK_KEY_B },         { "select", PORT_LINK_KEY_SELECT },
            { "start", PORT_LINK_KEY_START }, { "right", PORT_LINK_KEY_RIGHT }, { "left", PORT_LINK_KEY_LEFT },
            { "up", PORT_LINK_KEY_UP },       { "down", PORT_LINK_KEY_DOWN },   { "r", PORT_LINK_KEY_R },
            { "l", PORT_LINK_KEY_L },         { "x", PORT_LINK_KEY_X },         { "y", PORT_LINK_KEY_Y },
        };
        uint32_t keys = 0;
        const char* list = query ? strstr(query, "keys=") : NULL;
        if (list != NULL) {
            list += 5;
            while (*list && *list != '&') {
                size_t n = strcspn(list, ",&");
                for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
                    if (strlen(kNames[i].name) == n && strncasecmp(list, kNames[i].name, n) == 0) {
                        keys |= kNames[i].key;
                    }
                }
                list += n;
                if (*list == ',') {
                    ++list;
                }
            }
        }
        const int frames = QueryInt(query, "frames", 6);
        sInjectKeys = keys;
        sInjectTotal = sInjectFrames = keys ? (unsigned)(frames < 1 ? 1 : frames > 240 ? 240 : frames) : 0;
        RespondText(client, keys ? 200 : 400, keys ? "ok" : "no such button");
    } else if (post && strcmp(target, "/launcher") == 0) {
        /* restore=1: the launcher the console came with, back again. */
        FILE* backup = fopen(LAUNCHER_BACKUP, "rb");
        if (backup == NULL) {
            RespondText(client, 404, "no " LAUNCHER_BACKUP);
        } else {
            fclose(backup);
            remove(LAUNCHER_PATH);
            const bool ok = rename(LAUNCHER_BACKUP, LAUNCHER_PATH) == 0;
            LinkLog("[link] launcher restored: %s", ok ? "ok" : "failed");
            RespondText(client, ok ? 200 : 500, ok ? "ok, the old launcher is back" : "cannot rename");
        }
    } else if (post && strcmp(target, "/quit") == 0) {
        /* Back to the Homebrew Launcher, whose netloader (3dslink) can then
         * take and start the next build. */
        sQuitIn = 30;
        RespondText(client, 200, "ok, quitting");
    } else if (post && strcmp(target, "/highlight") == 0) {
        /* The PC editor's "show the selection": the frame on the top screen
         * goes, the selection stays. */
        sHighlightHidden = QueryInt(query, "show", 1) == 0;
        RespondText(client, 200, "ok");
    } else if (post && strcmp(target, "/select") == 0) {
        AnswerSelect(client, query, body, bodyLength);
    } else if (post && strcmp(target, "/file") == 0) {
        RespondText(client, 400, "upload not started");
    } else if (post && strcmp(target, "/test") == 0) {
        if (QueryInt(query, "on", -1) >= 0) {
            SetTestMode(QueryInt(query, "on", 0) != 0);
        }
        if (QueryInt(query, "noclip", -1) >= 0) {
            sNoclipWanted = QueryInt(query, "noclip", 0) != 0;
        }
        char text[64];
        snprintf(text, sizeof(text), "{\"test\":%s,\"noclip\":%s}", sTestMode ? "true" : "false",
                 sNoclipWanted ? "true" : "false");
        Respond(client, 200, "application/json", text, strlen(text));
    } else if (post && strcmp(target, "/screen") == 0) {
        /* A background's depth on this screen when it is not a room (menus,
         * the world map); without depth= it goes back to its own. */
        const int bg = QueryInt(query, "bg", -1), tile = QueryInt(query, "tile", -1);
        const bool set = query != NULL && strstr(query, "depth=") != NULL;
        /* key=<hex>: a screen saved earlier, not the one on show now. */
        const char* keyText = query ? strstr(query, "key=") : NULL;
        const bool other = keyText != NULL;
        const u32 key = other ? (u32)strtoul(keyText + 4, NULL, 16) : Port_Stereo_ScreenKey();
        if ((bg < 0 || bg > 3) && (tile < 0 || tile > 0x3ff)) {
            RespondText(client, 400, "bg=0..3 or tile=0..1023");
        } else if (!other && Port_Stereo_InRoom()) {
            RespondText(client, 409, "not a menu screen");
        } else {
            if (tile >= 0) {
                PortStereoEdits_SetScreenObjDepth(key, tile, set, QueryInt(query, "depth", 0));
            } else {
                PortStereoEdits_SetScreenDepth(key, bg, set, QueryInt(query, "depth", 0));
            }
            PortStereoEdits_Save();
            RespondText(client, 200, "ok");
        }
    } else if (post && strcmp(target, "/sweep") == 0) {
        RespondText(client, SweepStart() ? 202 : 409, sSweep.active ? "sweeping" : "not now");
    } else if (post && strcmp(target, "/remove") == 0) {
        /* An old test build off the card; never the one running (romfs). */
        char name[64] = { 0 }, path[96];
        const char* at = query ? strstr(query, "name=") : NULL;
        if (at != NULL) {
            sscanf(at + 5, "%63[A-Za-z0-9._-]", name);
        }
        const size_t n = strlen(name);
        snprintf(path, sizeof(path), "sdmc:/3ds/%s", name);
        const bool ok = n > 5 && strcmp(name + n - 5, ".3dsx") == 0 && remove(path) == 0;
        RespondText(client, ok ? 200 : 400, ok ? "ok" : "cannot remove");
    } else if (post && strcmp(target, "/relaunch") == 0) {
        char name[64] = { 0 };
        const char* at = query ? strstr(query, "name=") : NULL;
        if (at != NULL) {
            sscanf(at + 5, "%63[A-Za-z0-9._-]", name);
        }
        const size_t n = strlen(name);
        /* Relaunching through the Homebrew Launcher's loader hung the
         * console more than once: the user restarts by hand. */
        const int status = n > 5 && strcmp(name + n - 5, ".3dsx") == 0 ? 409 : 400;
        LinkLog("[link] relaunch %s: %d", name, status);
        if (status == 200) {
            PortStereoEdits_Save();
            FILE* marker = fopen(PORT_STEREO_LINK_RELAUNCH_MARKER, "w");
            if (marker != NULL) {
                fclose(marker);
            }
            sQuitIn = 45; /* let this answer leave first */
        }
        RespondText(client, status, status == 200 ? "ok, restarting" : status == 404 ? "no such file"
                                    : status == 409 ? "the Homebrew Launcher cannot take it now: restart by hand"
                                                    : "cannot");
    } else if (post && strcmp(target, "/save") == 0) {
        const bool saved = PortStereoEdits_Save();
        RespondText(client, saved ? 200 : 400, saved ? "ok" : "cannot write");
    } else if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
        AnswerEditorPage(client);
    } else {
        RespondText(client, 404, "no such thing");
    }
}

/* Whether the whole request -- headers and Content-Length bytes of body --
 * has arrived. */
static bool RequestComplete(const Client* client) {
    const char* headerEnd = strstr(client->request, "\r\n\r\n");
    if (headerEnd == NULL) {
        return false;
    }
    size_t contentLength = 0;
    for (const char* p = client->request; p < headerEnd; ++p) {
        if ((p == client->request || p[-1] == '\n') && strncasecmp(p, "Content-Length:", 15) == 0) {
            contentLength = (size_t)strtoul(p + 15, NULL, 10);
        }
    }
    return client->requestLength >= (size_t)(headerEnd + 4 - client->request) + contentLength;
}

static void ServeClient(Client* client) {
    if (client->draining) {
        char sink[256];
        const ssize_t n = recv(client->socket, sink, sizeof(sink), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || ++client->draining > DRAIN_FRAMES) {
            CloseClient(client);
        }
        return;
    }
    if (client->upload != NULL) {
        static char chunk[RECV_CHUNK];
        /* The game holds still while a build comes in (port_bios.c), so a
         * frame can take more. */
        size_t budget = UPLOAD_PER_TICK * 4;
        const unsigned long long started = Platform3DS_SystemTick();
        while (client->uploadLeft > 0 && budget > 0 &&
               Platform3DS_SystemTick() - started < UPLOAD_TICKS_PER_FRAME) {
            const ssize_t n = recv(client->socket, chunk, sizeof(chunk) < budget ? sizeof(chunk) : budget, 0);
            if (n > 0) {
                UploadWrite(client, chunk, (size_t)n);
                budget -= (size_t)n < budget ? (size_t)n : budget;
                client->age = 0; /* only data keeps an upload alive */
                continue;
            }
            if (n == 0) {
                CloseClient(client);
                return;
            }
            break;
        }
        if (client->age > UPLOAD_STALL_FRAMES) {
            /* The sender went quiet: drop it and let the game go on. */
            LinkLog("[link] upload of %s stalled at %u bytes, dropped", client->uploadPath,
                    (unsigned)client->uploadDone);
            CloseClient(client);
            return;
        }
        if (client->uploadLeft == 0) {
            UploadFinish(client);
        } else {
            return;
        }
    }
    if (client->response == NULL) {
        for (;;) {
            if (client->requestLength + RECV_CHUNK + 1 > client->requestCapacity) {
                const size_t capacity = client->requestCapacity ? client->requestCapacity * 2 : 64 * 1024;
                char* grown = capacity <= MAX_REQUEST ? realloc(client->request, capacity) : NULL;
                if (grown == NULL) {
                    CloseClient(client);
                    return;
                }
                client->request = grown;
                client->requestCapacity = capacity;
            }
            const ssize_t n = recv(client->socket, client->request + client->requestLength, RECV_CHUNK, 0);
            if (n > 0) {
                client->requestLength += (size_t)n;
                client->request[client->requestLength] = '\0';
                /* POST /file: once the headers are in, the body goes to the card. */
                const char* end = strstr(client->request, "\r\n\r\n");
                const bool launcher = end != NULL && strncmp(client->request, "POST /launcher", 14) == 0 &&
                                      strstr(client->request, "restore=1") == NULL;
                if (end != NULL && (strncmp(client->request, "POST /file", 10) == 0 || launcher)) {
                    char target[256] = { 0 };
                    sscanf(client->request, "%*7s %255s", target);
                    char* query = strchr(target, '?');
                    size_t contentLength = 0;
                    for (const char* line = client->request; line != NULL && line < end;) {
                        if (strncasecmp(line, "Content-Length:", 15) == 0) {
                            contentLength = (size_t)strtoul(line + 15, NULL, 10);
                        }
                        line = strchr(line, '\n');
                        line = line ? line + 1 : NULL;
                    }
                    const size_t head = (size_t)(end + 4 - client->request);
                    if (UploadStart(client, query ? query + 1 : NULL, contentLength, launcher)) {
                        UploadWrite(client, client->request + head, client->requestLength - head);
                        free(client->request);
                        client->request = NULL;
                        client->requestLength = client->requestCapacity = 0;
                        ServeClient(client);
                    }
                    return;
                }
                continue;
            }
            if (n == 0) {
                CloseClient(client); /* closed before asking */
                return;
            }
            break; /* EAGAIN: nothing more yet */
        }
        if (client->request == NULL || !RequestComplete(client)) {
            return;
        }
        sLastRequestFrame = sFrame;
        sRequestSeen = true;
        if (!client->frameWait) {
            Answer(client);
        }
        if (client->socket < 0) {
            return;
        }
        if (client->frameWait) {
            const uint16_t *left, *right;
            unsigned stride, x0, y0;
            if (PortStereoLink_FrameReady(&left, &right, &stride, &x0, &y0)) {
                Buffer b = { 0 };
                Put(&b, "TMCF", 4);
                PutU16(&b, 240);
                PutU16(&b, 160);
                for (int eye = 0; eye < 2; ++eye) {
                    const uint16_t* pixels = eye ? right : left;
                    for (unsigned y = 0; y < 160; ++y) {
                        Put(&b, pixels + (size_t)(y0 + y) * stride + x0, 240 * sizeof(uint16_t));
                    }
                }
                client->frameWait = 0;
                RespondBuffer(client, "application/octet-stream", &b);
            } else if (++client->frameWait > 90) {
                client->frameWait = 0;
                RespondText(client, 409, "no picture (3D slider down or software renderer)");
            } else {
                return;
            }
        }
    }
    size_t budget = SEND_PER_TICK;
    while (client->responseSent < client->responseLength && budget > 0) {
        size_t chunk = client->responseLength - client->responseSent;
        if (chunk > budget) {
            chunk = budget;
        }
        const ssize_t n = send(client->socket, client->response + client->responseSent, chunk, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            CloseClient(client);
            return;
        }
        client->responseSent += (size_t)n;
        budget -= (size_t)n;
    }
    if (client->responseSent >= client->responseLength) {
        shutdown(client->socket, SHUT_WR);
        client->draining = 1;
    }
}

extern void SetActiveSave(u32 idx);
extern void ResetSaveFile(u32 index);

static void PendingGotoTick(void) {
    if (!sPendingGoto.active) {
        return;
    }
    if (++sPendingGoto.frames > 60 * 40) {
        sPendingGoto.active = false; /* the game never got there */
        return;
    }
    if (gMain.task == TASK_TITLE) {
        /* Hold START a few frames out of every sixteen, as a hand would. */
        if ((sFrame & 15) < 3) {
            *(volatile u16*)(gIoMem + 0x130) &= (u16)~LINK_START_BUTTON;
        }
    } else if (gMain.task == TASK_FILE_SELECT) {
        /* The saves are read as the screen opens; give it a moment. */
        if (++sPendingGoto.fileSelectFrames < 60) {
            return;
        }
        int slot = gSaveHeader->saveFileId < NUM_SAVE_SLOTS ? gSaveHeader->saveFileId : 0;
        if (gFileSelectState.saveStatus[slot] != 1 /* SAVE_VALID */) {
            slot = -1;
            for (int i = 0; i < NUM_SAVE_SLOTS && slot < 0; ++i) {
                if (gFileSelectState.saveStatus[i] == 1) {
                    slot = i;
                }
            }
        }
        if (slot < 0) {
            /* No save at all: a new game that starts where the warp goes. */
            slot = 0;
            ResetSaveFile(0);
            SaveFile* save = &gFileSelectState.saves[0];
            save->initialized = 1;
            save->name[0] = 'A';
            save->saved_status.area_next = (u8)sPendingGoto.area;
            save->saved_status.room_next = (u8)sPendingGoto.room;
            gFileSelectState.saveStatus[0] = 1;
        }
        SetActiveSave((u32)slot);
        SetTask(TASK_GAME);
    } else if (!InGame() || gFadeControl.active || gRoomControls.scrollAction > 1) {
        sPendingGoto.settledFrames = 0;
    } else if (++sPendingGoto.settledFrames >= 90) {
        /* A moment after the saved room is up: until then the game is still
         * setting it up and would undo the warp. Link need not have control
         * (a message may be open); a plain /goto does not ask for it either. */
        if (PortStereoLink_Goto(sPendingGoto.area, sPendingGoto.room, sPendingGoto.x, sPendingGoto.y,
                                sPendingGoto.layer) != 409) {
            sPendingGoto.active = false;
        }
    }
}

void PortStereoLink_Tick(void) {
    ++sFrame;
    /* Keep the background graphics of what is on screen, twice a second. */
    if (sEnabled && sNetUp && (sFrame % 30) == 0) {
        CaptureVisibleChars();
    }
    SweepTick();
    if (sTestMode) {
        if (!sEnabled) {
            SetTestMode(false);
        } else if (InGame() && gSave.stats.health < gSave.stats.maxHealth) {
            gSave.stats.health = gSave.stats.maxHealth;
        }
    }
    PendingGotoTick();
    /* Link walks through walls while an editor is open, here or on the PC. */
    const bool pcEditor = sEnabled && sRequestSeen && sFrame - sLastRequestFrame < PC_EDITOR_GONE_FRAMES;
    Port_DebugAction_SetAutoNoclip(sNoclipWanted && (PortStereoEditor_IsOpen() || pcEditor));
    if (sQuitIn != 0 && --sQuitIn == 0) {
        LinkLog("[link] quitting for the relaunch");
        PortStereoLink_Quit();
    }
    if (!sEnabled) {
        if (sNetUp || sListen >= 0) {
            LinkStop();
        }
        sNetFailed = false;
        return;
    }
    if (sAsleep) {
        return;
    }
    if (sWakeWait != 0) {
        --sWakeWait;
        return;
    }
    if (sNetResetPending) {
        sNetResetPending = false;
        if (sNetUp) {
            PortStereoLink_NetDown();
            sNetUp = false;
        }
        LinkLog("[link] after sleep: network restarted");
    }
    if (sListen < 0) {
        /* Try again every few seconds: the Wi-Fi may come up later. */
        if (sFrame % 180 != 1 && sNetFailed) {
            return;
        }
        sNetFailed = false;
        if (!LinkStart()) {
            return;
        }
    }
    for (;;) {
        Client* free = NULL;
        for (int i = 0; i < MAX_CLIENTS && free == NULL; ++i) {
            if (sClients[i].socket < 0) {
                free = &sClients[i];
            }
        }
        if (free == NULL) {
            /* Every slot taken: give up the one that has answered longest ago. */
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (sClients[i].draining && (free == NULL || sClients[i].draining > free->draining)) {
                    free = &sClients[i];
                }
            }
            if (free == NULL) {
                break;
            }
            CloseClient(free);
        }
        struct sockaddr_in peer;
        socklen_t peerLength = sizeof(peer);
        const int socket = accept(sListen, (struct sockaddr*)&peer, &peerLength);
        if (socket < 0) {
            break;
        }
        SetNonBlocking(socket);
        /* A wider window: a 3DSX by POST /file otherwise trickles in. */
        const int window = 256 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &window, sizeof(window));
        *free = (Client){ .socket = socket };
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        Client* client = &sClients[i];
        if (client->socket < 0) {
            continue;
        }
        if (++client->age > CLIENT_TIMEOUT_FRAMES) {
            CloseClient(client);
            continue;
        }
        ServeClient(client);
    }
}

int PortStereoLink_QuitSeconds(void) {
    return sQuitIn ? (int)((sQuitAt - sFrame + 59u) / 60u) : -1;
}

bool PortStereoLink_UploadInfo(unsigned* received, unsigned* total, const char** doneName) {
    *doneName = (int)(sUploadedUntil - sFrame) > 0 ? sUploadedName : NULL;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        const Client* c = &sClients[i];
        if (c->socket >= 0 && c->upload != NULL) {
            *received = (unsigned)c->uploadDone;
            *total = (unsigned)(c->uploadDone + c->uploadLeft);
            return true;
        }
    }
    return false;
}

int PortStereoLink_UploadProgress(void) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        const Client* c = &sClients[i];
        if (c->socket >= 0 && c->upload != NULL) {
            const size_t total = c->uploadDone + c->uploadLeft;
            return total ? (int)((c->uploadDone * 1000u) / total) : 0;
        }
    }
    return -1;
}

/* The console going to sleep with sockets open (and a build half sent) did
 * not wake up again: everything network is closed before it sleeps, and the
 * link comes back by itself a few seconds after it wakes. Main thread, from
 * the APT hook. */
void PortStereoLink_Sleep(bool asleep) {
    if (asleep) {
        /* Only the sockets here, inside the APT hook: shutting the SOC
         * service down at this point (3D-34..39) was followed by a console
         * that never woke. The service is restarted after waking instead. */
        LinkLog("[link] sleep: closing sockets (net %s)", sNetUp ? "up" : "down");
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            CloseClient(&sClients[i]);
        }
        if (sListen >= 0) {
            close(sListen);
            sListen = -1;
        }
        sNetResetPending = sNetUp;
        sAsleep = true;
        sNetFailed = false;
        LinkLog("[link] sleep: sockets closed");
    } else if (sAsleep) {
        LinkLog("[link] woke up");
        sAsleep = false;
        sWakeWait = 60 * 3;
    }
}

uint32_t PortStereoLink_InjectedKeys(bool* first) {
    if (sInjectFrames == 0) {
        return 0;
    }
    *first = sInjectFrames == sInjectTotal;
    --sInjectFrames;
    return sInjectKeys;
}

void PortStereoLink_Shutdown(void) {
    sEnabled = false;
    LinkStop();
}
