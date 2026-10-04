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

#include <errno.h>
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
    MAX_REQUEST = 24 * 1024 * 1024, /* a 3dsx sent by POST /file */
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

/* Test mode, only while the link is on: every item and skill, hearts kept
 * full, optionally through walls; the save as it was is kept aside and comes
 * back when the mode ends, and nothing is written to the card meanwhile. */
static bool sTestMode;
static SaveFile* sTestBackup;
extern void Port_DebugAction_GiveAllItems(void);
extern void Port_DebugAction_SetNoclip(int on);
extern int Port_DebugQuery_Noclip(void);
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
        if (sPort != PORT_STEREO_LINK_PORT && n > 0 && (size_t)n < size) {
            snprintf(out + n, size - (size_t)n, ":%d", sPort);
        }
    }
}

static void SetNonBlocking(int socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

static void CloseClient(Client* client) {
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
static u8* sRoomChars;   /* [layer][row * SIDE + col][32] */
static u8* sRoomCharsHave; /* [layer][row * SIDE + col] */
static int sRoomCharsRoom = -1;

static void CaptureVisibleChars(void) {
    const int room = (gRoomControls.area << 8) | gRoomControls.room;
    if (!InGame() || gRoomControls.scrollAction > 1) {
        return;
    }
    if (sRoomChars == NULL) {
        sRoomChars = malloc(2u * SIDE * SIDE * CHAR_BYTES);
        sRoomCharsHave = malloc(2u * SIDE * SIDE);
        if (sRoomChars == NULL || sRoomCharsHave == NULL) {
            free(sRoomChars);
            free(sRoomCharsHave);
            sRoomChars = sRoomCharsHave = NULL;
            return;
        }
        sRoomCharsRoom = -1;
    }
    if (sRoomCharsRoom != room) {
        memset(sRoomCharsHave, 0, 2u * SIDE * SIDE);
        sRoomCharsRoom = room;
    }
    const int x0 = (int)gRoomControls.scroll_x - (int)gRoomControls.origin_x;
    const int y0 = (int)gRoomControls.scroll_y - (int)gRoomControls.origin_y;
    const int cols = gRoomControls.width / 8, rows = gRoomControls.height / 8;
    for (int layer = 0; layer < 2; ++layer) {
        const MapLayer* map = layer ? &gMapTop : &gMapBottom;
        if (map->bgSettings == NULL) {
            continue;
        }
        const u32 base = ((map->bgSettings->control >> 2) & 3) * 0x4000u;
        for (int row = y0 >> 3; row <= (y0 + 159) >> 3; ++row) {
            for (int col = x0 >> 3; col <= (x0 + 239) >> 3; ++col) {
                if (row < 0 || col < 0 || row >= rows || col >= cols || row >= SIDE || col >= SIDE) {
                    continue;
                }
                const u32 tile = (u32)(col >> 1) | ((u32)(row >> 1) << 6);
                const u32 index = Port_Stereo_TileDrawIndex(layer, tile);
                const u16 entry = map->subTiles[index * 4 + ((col & 1) | ((row & 1) << 1))];
                const u32 at = (u32)layer * SIDE * SIDE + (u32)row * SIDE + (u32)col;
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
        memcpy(shot->oam, gOamMem, sizeof(shot->oam));
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

static void Put(Buffer* b, const void* data, size_t length) {
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
static void PutF(Buffer* b, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n > 0) {
        Put(b, text, (size_t)(n < (int)sizeof(text) ? n : (int)sizeof(text) - 1));
    }
}

static void PutU16(Buffer* b, unsigned value) {
    const u8 bytes[2] = { (u8)value, (u8)(value >> 8) };
    Put(b, bytes, 2);
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
    Respond(client, 200, type, b->data, b->length);
    free(b->data);
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

static void AnswerStatus(Client* client) {
    Buffer b = { 0 };
    const RoomHeader* header = RoomHeaderOf(gRoomControls.area, gRoomControls.room);
    PutF(&b,
         "{\"inGame\":%s,\"live\":%s,\"area\":%u,\"room\":%u,\"width\":%u,\"height\":%u,"
         "\"originX\":%u,\"originY\":%u,\"scrollX\":%d,\"scrollY\":%d,\"linkX\":%d,\"linkY\":%d,"
         "\"tileset\":%u,\"transition\":%s,\"rev\":%lu,\"frame\":%u,\"selRev\":%u,\"editor\":%s,"
         "\"fade\":%s,\"starting\":%s,\"task\":%u,\"test\":%s,\"noclip\":%s,\"health\":%u,\"maxHealth\":%u,"
         "\"hudMax\":%u,\"sweep\":%s,\"sweepDone\":%d,\"sweepRoom\":%d}",
         InGame() ? "true" : "false", Port_Stereo_ReliefLive() ? "true" : "false", gRoomControls.area,
         gRoomControls.room, gRoomControls.width, gRoomControls.height, gRoomControls.origin_x,
         gRoomControls.origin_y, gRoomControls.scroll_x, gRoomControls.scroll_y,
         (int)gPlayerEntity.base.x.HALF.HI - (int)gRoomControls.origin_x,
         (int)gPlayerEntity.base.y.HALF.HI - (int)gRoomControls.origin_y, header ? header->tileSet_id : 0u,
         gRoomControls.scrollAction > 1 ? "true" : "false", (unsigned long)PortStereoEdits_Revision(), sFrame,
         PortStereoEditor_SelectionRevision(), PortStereoEditor_IsOpen() ? "true" : "false",
         gFadeControl.active ? "true" : "false", sPendingGoto.active ? "true" : "false", gMain.task,
         sTestMode ? "true" : "false", Port_DebugQuery_Noclip() ? "true" : "false", gSave.stats.health,
         gSave.stats.maxHealth, gHUD.maxHealth, sSweep.active ? "true" : "false", sSweep.shotCount, sSweep.room);
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
    Put(&b, gOamMem, 0x400);
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
        const int cols = view.cols < SIDE ? view.cols : SIDE, rows = view.rows < SIDE ? view.rows : SIDE;
        Put(&b, "CHRS", 4);
        PutU16(&b, (unsigned)cols);
        PutU16(&b, (unsigned)rows);
        for (int layer = 0; layer < 2; ++layer) {
            for (int row = 0; row < rows; ++row) {
                Put(&b, sRoomCharsHave + (size_t)layer * SIDE * SIDE + (size_t)row * SIDE, (size_t)cols);
            }
            for (int row = 0; row < rows; ++row) {
                Put(&b, sRoomChars + ((size_t)layer * SIDE * SIDE + (size_t)row * SIDE) * CHAR_BYTES,
                    (size_t)cols * CHAR_BYTES);
            }
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
static void AnswerFile(Client* client, const char* query, const char* body, size_t bodyLength) {
    char name[64] = { 0 };
    const char* at = query ? strstr(query, "name=") : NULL;
    if (at != NULL) {
        sscanf(at + 5, "%63[A-Za-z0-9._-]", name);
    }
    const size_t n = strlen(name);
    if (n < 6 || strcmp(name + n - 5, ".3dsx") != 0 || strstr(name, "..") != NULL || bodyLength < 4 ||
        memcmp(body, "3DSX", 4) != 0) {
        RespondText(client, 400, "only a .3dsx, by name, into sdmc:/3ds/");
        return;
    }
    char path[96], temp[104];
    snprintf(path, sizeof(path), "sdmc:/3ds/%s", name);
    snprintf(temp, sizeof(temp), "%s.part", path);
    LinkLog("[link] writing %s, %u bytes", path, (unsigned)bodyLength);
    FILE* file = fopen(temp, "wb");
    bool written = file != NULL && fwrite(body, 1, bodyLength, file) == bodyLength;
    if (file != NULL && fclose(file) != 0) {
        written = false;
    }
    if (!written) {
        remove(temp);
        RespondText(client, 400, "cannot write");
        return;
    }
    remove(path);
    RespondText(client, rename(temp, path) == 0 ? 200 : 400, "ok");
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
int PortStereoLink_Highlight(float (*rects)[4], int max) {
    if (!sEnabled || sHighlightCount == 0 || sHighlightRoom != ((gRoomControls.area << 8) | gRoomControls.room) ||
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
    } else if (strcmp(target, "/entities") == 0) {
        AnswerEntities(client);
    } else if (strcmp(target, "/edits") == 0) {
        AnswerEdits(client, query, post, body, bodyLength);
    } else if (post && strcmp(target, "/goto") == 0) {
        AnswerGoto(client, query);
    } else if (post && strcmp(target, "/select") == 0) {
        AnswerSelect(client, query, body, bodyLength);
    } else if (post && strcmp(target, "/file") == 0) {
        AnswerFile(client, query, body, bodyLength);
    } else if (post && strcmp(target, "/test") == 0) {
        if (QueryInt(query, "on", -1) >= 0) {
            SetTestMode(QueryInt(query, "on", 0) != 0);
        }
        if (QueryInt(query, "noclip", -1) >= 0) {
            Port_DebugAction_SetNoclip(sTestMode && QueryInt(query, "noclip", 0) != 0);
        }
        char text[64];
        snprintf(text, sizeof(text), "{\"test\":%s,\"noclip\":%s}", sTestMode ? "true" : "false",
                 Port_DebugQuery_Noclip() ? "true" : "false");
        Respond(client, 200, "application/json", text, strlen(text));
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
        const int status = n > 5 && strcmp(name + n - 5, ".3dsx") == 0 ? PortStereoLink_Relaunch(name) : 400;
        LinkLog("[link] relaunch %s: %d", name, status);
        if (status == 200) {
            PortStereoEdits_Save();
            sQuitIn = 45; /* let this answer leave first */
        }
        RespondText(client, status, status == 200 ? "ok, restarting" : status == 404 ? "no such file"
                                    : status == 409 ? "not from the Homebrew Launcher" : "cannot");
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
    if (sQuitIn != 0 && --sQuitIn == 0) {
        LinkStop(); /* leave the port free for the build that starts next */
        PortStereoLink_Quit();
    }
    if (!sEnabled) {
        if (sNetUp || sListen >= 0) {
            LinkStop();
        }
        sNetFailed = false;
        return;
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

void PortStereoLink_Shutdown(void) {
    sEnabled = false;
    LinkStop();
}
