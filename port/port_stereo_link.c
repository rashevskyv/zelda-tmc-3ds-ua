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
 */
#include "port_stereo_link.h"
#include "port_stereo.h"
#include "port_stereo_edits.h"

#include "global.h"
#include "area.h"
#include "entity.h"
#include "main.h"
#include "map.h"
#include "player.h"
#include "room.h"
#include "scroll.h"
#include "transitions.h"
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

/* Areas 0x00..0x8F, as port_rom.c resolves them. */
#define LINK_AREA_COUNT 0x90

enum {
    MAX_CLIENTS = 4,
    MAX_REQUEST = 2 * 1024 * 1024,
    SEND_PER_TICK = 96 * 1024,
    CLIENT_TIMEOUT_FRAMES = 60 * 20,
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
} Client;

static volatile bool sEnabled;
static bool sNetUp;
static bool sNetFailed;
static uint32_t sAddress;
static int sListen = -1;
static Client sClients[MAX_CLIENTS];
static unsigned sFrame;

/* Cells the editor points at: area << 8 | room, and runs of a row. */
static int sHighlightRoom = -1;
static struct {
    u8 row, col0, col1;
} sHighlight[MAX_HIGHLIGHT];
static int sHighlightCount;

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
        snprintf(out, size, "%u.%u.%u.%u", (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                 (unsigned)((a >> 16) & 0xff), (unsigned)(a >> 24));
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
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT_STEREO_LINK_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const int yes = 1;
    setsockopt(sListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(sListen, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(sListen, 4) != 0) {
        LinkStop();
        sNetFailed = true;
        return false;
    }
    SetNonBlocking(sListen);
    return true;
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
    const char* reason = status == 200 ? "OK" : status == 204 ? "No Content" : status == 404 ? "Not Found"
                       : status == 409 ? "Conflict" : "Bad Request";
    char head[384];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.0 %d %s\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n"
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
         "\"tileset\":%u,\"transition\":%s,\"rev\":%lu,\"frame\":%u}",
         InGame() ? "true" : "false", Port_Stereo_ReliefLive() ? "true" : "false", gRoomControls.area,
         gRoomControls.room, gRoomControls.width, gRoomControls.height, gRoomControls.origin_x,
         gRoomControls.origin_y, gRoomControls.scroll_x, gRoomControls.scroll_y,
         (int)gPlayerEntity.base.x.HALF.HI - (int)gRoomControls.origin_x,
         (int)gPlayerEntity.base.y.HALF.HI - (int)gRoomControls.origin_y, header ? header->tileSet_id : 0u,
         gRoomControls.scrollAction > 1 ? "true" : "false", (unsigned long)PortStereoEdits_Revision(), sFrame);
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
    Put(&b, gBgPltt, 0x200);
    PutGrid(&b, view.autoGround, &view);
    PutGrid(&b, view.autoHeight, &view);
    PutGrid(&b, view.autoHeightTop, &view);
    PutGrid(&b, view.kind, &view);
    Put(&b, view.tileHash[0], sizeof(u32) * 64 * 64);
    Put(&b, view.tileHash[1], sizeof(u32) * 64 * 64);
    EntityOut entities = { &b, 0, false };
    VisitEntities(&entities);
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

static void AnswerGoto(Client* client, const char* query) {
    const int area = QueryInt(query, "area", -1), room = QueryInt(query, "room", -1);
    const RoomHeader* header = RoomHeaderOf(area, room);
    if (header == NULL) {
        RespondText(client, 404, "no such room");
        return;
    }
    if (!InGame() || gRoomControls.scrollAction > 1 || gRoomTransition.transitioningOut) {
        RespondText(client, 409, "busy");
        return;
    }
    Transition t = { 0 };
    t.warp_type = WARP_TYPE_AREA;
    t.area = (u8)area;
    t.room = (u8)room;
    t.endX = (u16)QueryInt(query, "x", header->pixel_width / 2);
    t.endY = (u16)QueryInt(query, "y", header->pixel_height / 2);
    t.layer = (u8)QueryInt(query, "layer", 1);
    gRoomTransition.stairs_idx = 0;
    DoExitTransition(&t);
    RespondText(client, 200, "ok");
}

static void AnswerSelect(Client* client, const char* query, const char* body, size_t bodyLength) {
    const int area = QueryInt(query, "area", gRoomControls.area);
    const int room = QueryInt(query, "room", gRoomControls.room);
    sHighlightCount = 0;
    sHighlightRoom = (area << 8) | room;
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

int PortStereoLink_Highlight(float (*rects)[4], int max) {
    if (!sEnabled || sHighlightRoom != ((gRoomControls.area << 8) | gRoomControls.room) || !InGame()) {
        return 0;
    }
    const int x0 = (int)gRoomControls.origin_x - (int)gRoomControls.scroll_x;
    const int y0 = (int)gRoomControls.origin_y - (int)gRoomControls.scroll_y;
    int n = 0;
    for (int i = 0; i < sHighlightCount && n < max; ++i) {
        const float x = (float)(x0 + sHighlight[i].col0 * 8), y = (float)(y0 + sHighlight[i].row * 8);
        const float w = (float)((sHighlight[i].col1 - sHighlight[i].col0 + 1) * 8);
        if (x + w <= 0.0f || x >= 240.0f || y + 8.0f <= 0.0f || y >= 160.0f) {
            continue;
        }
        rects[n][0] = x;
        rects[n][1] = y;
        rects[n][2] = w;
        rects[n][3] = 8.0f;
        ++n;
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
    } else if (strcmp(target, "/entities") == 0) {
        AnswerEntities(client);
    } else if (strcmp(target, "/edits") == 0) {
        AnswerEdits(client, query, post, body, bodyLength);
    } else if (post && strcmp(target, "/goto") == 0) {
        AnswerGoto(client, query);
    } else if (post && strcmp(target, "/select") == 0) {
        AnswerSelect(client, query, body, bodyLength);
    } else if (post && strcmp(target, "/save") == 0) {
        const bool saved = PortStereoEdits_Save();
        RespondText(client, saved ? 200 : 400, saved ? "ok" : "cannot write");
    } else if (strcmp(target, "/") == 0) {
        RespondText(client, 200, "The Minish Cap 3DS - stereo 3D link. Open the PC editor and connect here.");
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
    if (client->response == NULL) {
        for (;;) {
            if (client->requestLength + 4096 + 1 > client->requestCapacity) {
                const size_t capacity = client->requestCapacity ? client->requestCapacity * 2 : 8192;
                char* grown = capacity <= MAX_REQUEST ? realloc(client->request, capacity) : NULL;
                if (grown == NULL) {
                    CloseClient(client);
                    return;
                }
                client->request = grown;
                client->requestCapacity = capacity;
            }
            const ssize_t n = recv(client->socket, client->request + client->requestLength, 4096, 0);
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
        Answer(client);
        if (client->socket < 0) {
            return;
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
        CloseClient(client);
    }
}

void PortStereoLink_Tick(void) {
    ++sFrame;
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
            break;
        }
        struct sockaddr_in peer;
        socklen_t peerLength = sizeof(peer);
        const int socket = accept(sListen, (struct sockaddr*)&peer, &peerLength);
        if (socket < 0) {
            break;
        }
        SetNonBlocking(socket);
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
