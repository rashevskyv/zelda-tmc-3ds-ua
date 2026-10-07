/**
 * @file port_stereo.c
 * @brief Stereoscopic 3D relief: how high each cell of the room stands.
 *
 * A room is two flat backgrounds, so a renderer that draws layers apart for
 * two eyes shows walls, trees and furniture lying in the floor, and a yard on
 * a rise as deep as the field below it. The game does know better, and says
 * so per 16x16 tile: MapLayer.collisionData holds which 8x8 quarters block
 * movement, and MapLayer.actTiles what a tile is -- water, a ramp, or a ledge
 * Link can hop off and which way he lands.
 *
 * port_stereo_relief.h turns that into a height for every cell of the room.
 * This file feeds it the room, cuts the part in view into a grid over each
 * background's own tilemap cells -- the unit the renderer draws -- and tells
 * the sprite code how high the ground under a sprite is, so things that walk
 * stand on their ground instead of at one depth for the whole room.
 */
#include "port_stereo.h"
#include "port_stereo_relief.h"
#include "port_stereo_edits.h"
#include "port_gba_mem.h"
#include "cpu/mode1.h"

#include "global.h"
#include "entity.h"
#include "main.h"
#include "map.h"
#include "room.h"
#include "screen.h"
#include "menu.h"
#include "game.h"
#include "port_widescreen.h"
#include "area.h"

#include <string.h>

extern bool Port_Config_Get3DSStereoRelief(void);
extern float PlatformGpu3DS_StereoDepth(void);

u8 gPortStereoRelief[PORT_STEREO_RELIEF_LAYERS][PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
int gPortStereoReliefBg[PORT_STEREO_RELIEF_LAYERS] = { -1, -1 };
int gPortStereoReliefSink;

/* The room as last measured: a map is at most 64 tiles a side, two cells to a
 * tile. Heights are in depth units from the reference ground. */
enum { ROOM_SIDE = 128, ROOM_CELLS = ROOM_SIDE * ROOM_SIDE, REMEASURE_FRAMES = 64 };
static u8 sKind[ROOM_CELLS];
static u8 sTopSolid[ROOM_CELLS];
static u16 sRegion[ROOM_CELLS];
static u16 sQueue[ROOM_CELLS];
static s8 sGround[ROOM_CELLS];
static s8 sHeight[ROOM_CELLS];
static s8 sHeightTop[ROOM_CELLS];
/* The same before the editors' corrections, for the PC editor. */
static s8 sAutoGround[ROOM_CELLS];
static s8 sAutoHeight[ROOM_CELLS];
static s8 sAutoHeightTop[ROOM_CELLS];
/* What each 16x16 map tile of the two layers looks like (tile rules). */
static u32 sTileHash[2][64 * 64];
static int sCols, sRows;
static int sArea = -1, sRoom = -1;
static u32 sAge;
static u32 sEditsRevision;
/* Whether the grids handed to the renderer this frame are in use; sprites
 * follow the ground only while the ground itself is drawn raised. */
static bool32 sLive;
static int sSpritePriority;

/* Whether one 8x8 quarter of a tile blocks movement, from the tile's collision
 * value (see IsTileCollision in src/movement.c, collisionType 0). 1..15 is a
 * mask of the four quarters. Larger values are shaped tiles: a few never
 * block, eight fill one quarter, and the rest are solid for all practical
 * purposes. */
static bool32 QuarterIsSolid(u32 collision, u32 x, u32 y) {
    if (collision == 0) {
        return FALSE;
    }
    if (collision < 0x10) {
        if ((y & 8) == 0) {
            collision >>= 2;
        }
        if ((x & 8) == 0) {
            collision >>= 1;
        }
        return collision & 1;
    }
    if (collision == 0x22 || collision == 0x28 || collision == 0x2a || (collision >= 0x50 && collision <= 0x5f)) {
        return FALSE;
    }
    if ((collision >= 0x10 && collision <= 0x13) || (collision >= 0x19 && collision <= 0x1c)) {
        /* north-west, north-east, south-west, south-east */
        const u32 corner = (collision - (collision >= 0x19 ? 0x19 : 0x10));
        return ((corner & 1) != 0) == ((x & 8) != 0) && ((corner & 2) != 0) == ((y & 8) != 0);
    }
    return TRUE;
}

/* What a cell is, from its tile's behaviour first (the ActTile values that
 * lead to each surface, see include/tiles.h and the hop tables gUnk_0811C1E8
 * in src/playerUtils.c) and its collision second. Deep water blocks movement
 * like a wall, so what a tile IS has to be asked before whether it blocks. */
static u8 CellKind(u32 tile, u32 x, u32 y) {
    switch (gMapBottom.actTiles[tile]) {
        case 0x2b:
        case 0x41:
            return PORT_STEREO_CELL_LEDGE_N;
        case 0x2c:
        case 0x42:
            return PORT_STEREO_CELL_LEDGE_E;
        case 0x2a:
        case 0x40:
            return PORT_STEREO_CELL_LEDGE_S;
        case 0x2d:
        case 0x43:
            return PORT_STEREO_CELL_LEDGE_W;
        case 0x26: /* -> SURFACE_SLOPE_GNDGND_V */
        case 0x27: /* -> SURFACE_SLOPE_GNDGND_H */
        case 0x34: /* -> SURFACE_LIGHT_GRADE */
        case 0x35: /* -> SURFACE_29, the same across */
            return PORT_STEREO_CELL_RAMP;
        case 0x0d: /* -> SURFACE_PIT */
        case 0x10: /* -> SURFACE_WATER; shallow water is ground with a film on it */
        case 0x19: /* -> SURFACE_HOLE */
        case 0xf0: /* -> SURFACE_HOLE */
            return PORT_STEREO_CELL_SUNKEN;
        default:
            return QuarterIsSolid(gMapBottom.collisionData[tile], x, y) ? PORT_STEREO_CELL_SOLID
                                                                        : PORT_STEREO_CELL_OPEN;
    }
}

/* BG3CNT of the cloud shadows: screen base 30, char base 1, priority 1. */
#define CLOUD_SHADOW_BGCNT 0x1e05

static int MapBackground(const MapLayer* layer) {
    const void* settings = layer->bgSettings;
    if (settings == (const void*)&gScreen.bg1) {
        return 1;
    }
    if (settings == (const void*)&gScreen.bg2) {
        return 2;
    }
    if (settings == (const void*)&gScreen.bg3) {
        return 3;
    }
    return -1;
}

/* Names the room's tile set by where its graphics come from: the source
 * offsets of its chain of map data definitions (12 bytes each, the high bit
 * of src saying another follows). Rooms and areas that load the same
 * graphics share the name, whichever room table they come from. */
static u32 TilesetKey(void) {
    const RoomResInfo* info = gArea.pCurrentRoomInfo;
    const u8* chain = info != NULL ? (const u8*)info->tileSet : NULL;
    u32 key = 2166136261u;
    if (chain == NULL) {
        return key;
    }
    for (int i = 0; i < 16; ++i) {
        u32 src;
        memcpy(&src, chain + 12 * i, sizeof(src));
        for (int b = 0; b < 4; ++b) {
            key = (key ^ ((src >> (8 * b)) & 0xff)) * 16777619u;
        }
        if ((src & 0x80000000u) == 0) {
            break;
        }
    }
    return key;
}

extern u32 GetTileSetIndexForSpecialTile(u32 tilePosAndLayer, u32 tileIndex);

u32 Port_Stereo_TileDrawIndex(int layer, u32 tilePos) {
    const MapLayer* map = layer ? &gMapTop : &gMapBottom;
    const u32 value = map->mapData[tilePos];
    if (value < 0x4000) {
        return value & (TILESET_SIZE - 1);
    }
    /* Special tiles (chests, pots, switches) draw what the room's tile
     * entities say, or the tile they replaced (RenderMapLayerToSubTileMap). */
    return (GetTileSetIndexForSpecialTile(((u32)(layer + 1) << 12) | tilePos, map->mapDataOriginal[tilePos]) / 4) &
           (TILESET_SIZE - 1);
}

/* The rule key of every map tile in the room, 0 for a layer that is not
 * shown. Tiles repeat, so each tile index is hashed once. */
static void HashTiles(const MapLayer* layer, u32 tilesetKey, u32* hashes) {
    const int layerIndex = layer == &gMapTop;
    static u32 sByIndex[TILESET_SIZE];
    static u8 sDone[TILESET_SIZE];
    memset(hashes, 0, sizeof(u32) * 64 * 64);
    if (layer->bgSettings == NULL) {
        return;
    }
    memset(sDone, 0, sizeof(sDone));
    const int tilesW = (sCols + 1) / 2, tilesH = (sRows + 1) / 2;
    for (int ty = 0; ty < tilesH && ty < 64; ++ty) {
        for (int tx = 0; tx < tilesW && tx < 64; ++tx) {
            const u32 index = Port_Stereo_TileDrawIndex(layerIndex, (u32)(tx | (ty << 6)));
            if (!sDone[index]) {
                sByIndex[index] = PortStereoEdits_TileHash(&layer->subTiles[index * 4], tilesetKey);
                sDone[index] = 1;
            }
            hashes[tx | (ty << 6)] = sByIndex[index];
        }
    }
}

/* Measures the whole room. A room changes while it is on screen -- a door
 * opens, a bush is cut, the water drops -- so this runs again every second or
 * so as well as on entering a room; the spans between are short enough that a
 * stale cell is not seen. */
static void MeasureRoom(void) {
    sCols = gRoomControls.width / 8;
    sRows = gRoomControls.height / 8;
    if (sCols > ROOM_SIDE) {
        sCols = ROOM_SIDE;
    }
    if (sRows > ROOM_SIDE) {
        sRows = ROOM_SIDE;
    }
    for (int row = 0; row < sRows; ++row) {
        for (int col = 0; col < sCols; ++col) {
            const u32 tile = (u32)(col >> 1) | ((u32)(row >> 1) << 6);
            const u32 x = (u32)col * 8, y = (u32)row * 8;
            const u8 kind = CellKind(tile, x, y);
            sKind[row * sCols + col] = kind;
            /* The top layer holds the upper parts of what stands on the bottom
             * one -- furniture, wall tops -- so it is a thing wherever either
             * map is solid, and stays with what it belongs to. */
            sTopSolid[row * sCols + col] =
                kind != PORT_STEREO_CELL_SUNKEN && QuarterIsSolid(gMapTop.collisionData[tile], x, y);
        }
    }
    const PortStereoRoom room = { sCols, sRows, sKind, sRegion, sQueue };
    PortStereo_RoomHeights(&room, sTopSolid, sGround, sHeight, sHeightTop);
    const size_t cells = (size_t)sCols * (size_t)sRows;
    memcpy(sAutoGround, sGround, cells);
    memcpy(sAutoHeight, sHeight, cells);
    memcpy(sAutoHeightTop, sHeightTop, cells);
    const u32 tilesetKey = TilesetKey();
    HashTiles(&gMapBottom, tilesetKey, sTileHash[0]);
    HashTiles(&gMapTop, tilesetKey, sTileHash[1]);
    sEditsRevision = PortStereoEdits_Revision();
    PortStereoEdits_ApplyRoom(gRoomControls.area, gRoomControls.room, sCols, sRows, sHeight, sHeightTop, sGround,
                              PORT_STEREO_RELIEF_UNKNOWN, sTileHash[0], sTileHash[1]);
}

/* The ground under an entity's feet, in units above the reference ground
 * (0 when unknown), and the room cell it stands in; false when the relief
 * does not place it. */
static bool32 EntityGround(const Entity* entity, int* ground, int* col, int* row) {
    if (!sLive || entity->spriteRendering.b3 != sSpritePriority) {
        return FALSE;
    }
    *col = ((int)entity->x.HALF.HI - (int)gRoomControls.origin_x) >> 3;
    *row = ((int)entity->y.HALF.HI - (int)gRoomControls.origin_y) >> 3;
    if (*col < 0 || *row < 0 || *col >= sCols || *row >= sRows) {
        return FALSE;
    }
    /* A thing that is itself an entity (a pot, a sign) sits on a solid cell;
     * the ground it stands on is the first ground below it. */
    int at = *row;
    int found = sGround[at * sCols + *col];
    for (int down = 0; found == PORT_STEREO_RELIEF_UNKNOWN && down < 4 && at + 1 < sRows; ++down) {
        found = sGround[++at * sCols + *col];
    }
    *ground = found == PORT_STEREO_RELIEF_UNKNOWN ? 0 : found;
    return TRUE;
}

/* Stereoscopic 3D depth for an entity's sprites: a unit in front of the
 * ground it stands on, wherever that ground is not the reference level, and
 * as much nearer again as the 3D editor says. */
static u8 EntityDepth(const Entity* entity) {
    int ground, col, row;
    if (!EntityGround(entity, &ground, &col, &row)) {
        return 0;
    }
    const int edit = PortStereoEdits_EntityDelta(gRoomControls.area, gRoomControls.room, entity->kind, entity->id,
                                                 entity->type, col, row);
    if (ground == 0 && edit == 0) {
        return 0;
    }
    int depth = 3 * sSpritePriority - 1 - ground - edit;
    if (depth < -MODE1_STEREO_DEPTH_NEAR) {
        depth = -MODE1_STEREO_DEPTH_NEAR;
    }
    if (depth < 0 && edit == 0) {
        depth = 0;
    }
    return PORT_STEREO_DEPTH(depth);
}

/* Its shadow lies on that ground, however near the editor brings it. */
static u8 EntityShadowDepth(const Entity* entity) {
    int ground, col, row;
    if (!EntityGround(entity, &ground, &col, &row)) {
        return 0;
    }
    const int depth = 3 * sSpritePriority - ground;
    return PORT_STEREO_DEPTH(depth < 0 ? 0 : depth);
}

/* Which screen this is when it is not a room: the task, its state, the main
 * state (a subtask when GAMEMAIN_SUBTASK), and the pause menu's page. */
u32 Port_Stereo_ScreenKey(void) {
    return ((u32)gMain.task << 24) | ((u32)gMain.state << 16) | ((u32)gMain.substate << 8) | (u32)gMenu.menuType;
}

bool32 Port_Stereo_InRoom(void) {
    return gMain.task == TASK_GAME && gMain.state == GAMETASK_MAIN && gMain.substate != GAMEMAIN_SUBTASK;
}

/* The editor's sprite depths for screens that are not a room, by object tile,
 * over whatever depth the sprites were drawn with (the title's cap and logo
 * set theirs in code). Runs as the OAM's depths are committed. */
static void ApplyScreenObjDepths(void) {
    if (Port_Stereo_InRoom()) {
        return;
    }
    const u32 key = Port_Stereo_ScreenKey();
    if (!PortStereoEdits_ScreenHasObjs(key)) {
        return;
    }
    for (int i = 0; i < MODE1_GBA_OAM_COUNT; ++i) {
        const u16 a0 = gOamMem[i * 4], a2 = gOamMem[i * 4 + 2];
        if (!(a0 & 0x100) && (a0 & 0x200)) {
            continue; /* hidden */
        }
        int depth;
        if (PortStereoEdits_ScreenObjDepth(key, a2 & 0x3ff, &depth)) {
            virtuappu_mode1_obj_stereo_depth[i] = PORT_STEREO_DEPTH(depth);
        }
    }
}

/* The editor's layer depths for screens that are not a room. */
static void ApplyScreenDepths(void) {
    static u8 sSet; /* backgrounds this set, to let go of later */
    const bool32 screen = !Port_Stereo_InRoom();
    const u32 key = Port_Stereo_ScreenKey();
    for (int bg = 0; bg < 4; ++bg) {
        int depth;
        if (screen && PortStereoEdits_ScreenDepth(key, bg, &depth)) {
            /* Every frame: the relief lets go of a background it sank by
             * setting it back, which may come after this. */
            Port_Stereo_SetBgDepth((unsigned)bg, PORT_STEREO_DEPTH(depth));
            sSet |= (u8)(1 << bg);
        } else if (sSet & (1 << bg)) {
            Port_Stereo_SetBgDepth((unsigned)bg, 0);
            sSet &= (u8)~(1 << bg);
        }
    }
}

void Port_Stereo_CommitRelief(void) {
    ApplyScreenDepths();
    virtuappu_mode1_obj_depth_hook = ApplyScreenObjDepths;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_BOTTOM] = -1;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_TOP] = -1;
    gPortStereoReliefSink = 0;
    sLive = FALSE;
    gPortStereoEntityGround = EntityDepth;
    gPortStereoEntityShadow = EntityShadowDepth;

    /* The drifting cloud shadows of the overworld are a blended overlay on
     * the third background, a priority above the ground they darken
     * (CloudOverlayManager). Left at that priority's depth they hover over
     * the field like a sheet of glass; a shadow lies on the ground. This holds
     * whether or not the relief is drawn. */
    {
        static bool32 tagged;
        const bool32 clouds = gMain.task == TASK_GAME && gMapBottom.bgSettings != NULL &&
                              gScreen.bg3.control == CLOUD_SHADOW_BGCNT && (gScreen.lcd.displayControl & DISPCNT_BG3_ON);
        if (clouds) {
            Port_Stereo_SetBgDepth(3, PORT_STEREO_DEPTH(3 * (gMapBottom.bgSettings->control & 3)));
            tagged = TRUE;
        } else if (tagged) {
            Port_Stereo_SetBgDepth(3, 0);
            tagged = FALSE;
        }
    }

    /* Only while the camera follows inside one room. A room-to-room scroll
     * streams two rooms through the tilemap, and menus and cutscenes point the
     * layers elsewhere; the tilemaps then no longer mirror these maps. And
     * only while it will be drawn: with the 3D slider up, relief on, and the
     * view no wider than the tilemap (the Wide aspect draws the layers by a
     * path that has no relief). */
    if (gMain.task != TASK_GAME || gRoomControls.scrollAction > 1 || !Port_Config_Get3DSStereoRelief() ||
        PlatformGpu3DS_StereoDepth() <= 0.0f || Port_Widescreen_GameplayViewWidth() > 240) {
        sArea = -1;
        return;
    }
    const int bottomBg = MapBackground(&gMapBottom);
    const int topBg = MapBackground(&gMapTop);
    if (bottomBg < 0 && topBg < 0) {
        sArea = -1;
        return;
    }

    if (sArea != gRoomControls.area || sRoom != gRoomControls.room || ++sAge >= REMEASURE_FRAMES ||
        sEditsRevision != PortStereoEdits_Revision()) {
        sArea = gRoomControls.area;
        sRoom = gRoomControls.room;
        sAge = 0;
        MeasureRoom();
    }

    /* The engine refills the tilemaps every 16 pixels of camera travel so
     * that cell (0, 1) is the tile the camera's top-left corner is in; in
     * between only the scroll registers move (UpdateScreenShake in
     * src/scroll.c). */
    const int baseCol = ((gRoomControls.scroll_x - gRoomControls.origin_x) & ~0xf) >> 3;
    const int baseRow = (((gRoomControls.scroll_y - gRoomControls.origin_y) & ~0xf) >> 3) - 1;

    /* The renderer can only bring a cell nearer than its layer, so the
     * bottom layer is drawn as deep as the lowest ground in reach of the view
     * (water, a hollow) and everything is counted up from there: the
     * reference ground does not move, whatever scrolls into view. */
    int lowest = 0;
    for (int row = 0; row < PORT_STEREO_RELIEF_ROWS; ++row) {
        const int roomRow = baseRow + row;
        if (roomRow < 0 || roomRow >= sRows) {
            continue;
        }
        for (int col = 0; col < PORT_STEREO_RELIEF_COLS; ++col) {
            const int roomCol = baseCol + col;
            if (roomCol >= 0 && roomCol < sCols && sHeight[roomRow * sCols + roomCol] < lowest) {
                lowest = sHeight[roomRow * sCols + roomCol];
            }
        }
    }
    /* Heights are measured from the floor, but the renderer raises a cell
     * from its own layer, and outdoors the top layer already stands a whole
     * priority nearer than the floor. That head start comes off its cells, so
     * a tree top stands as high above the ground as its trunk says. */
    const int bottomPriority = bottomBg >= 0 ? (gMapBottom.bgSettings->control & 3) : 2;
    const int headStart = topBg >= 0 ? 3 * (bottomPriority - (int)(gMapTop.bgSettings->control & 3)) : 0;
    u8* bottom = gPortStereoRelief[PORT_STEREO_RELIEF_BOTTOM];
    u8* top = gPortStereoRelief[PORT_STEREO_RELIEF_TOP];
    for (int row = 0; row < PORT_STEREO_RELIEF_ROWS; ++row) {
        const int roomRow = baseRow + row;
        for (int col = 0; col < PORT_STEREO_RELIEF_COLS; ++col, ++bottom, ++top) {
            const int roomCol = baseCol + col;
            *bottom = *top = 0;
            if (roomRow < 0 || roomRow >= sRows || roomCol < 0 || roomCol >= sCols) {
                continue;
            }
            const int at = roomRow * sCols + roomCol;
            *bottom = (u8)PortStereo_Clamp(sHeight[at] - lowest, 0, PORT_STEREO_RELIEF_MAX_CELL);
            *top = (u8)PortStereo_Clamp(sHeightTop[at] - headStart, 0, PORT_STEREO_RELIEF_MAX_CELL);
        }
    }
    gPortStereoReliefBg[PORT_STEREO_RELIEF_BOTTOM] = bottomBg;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_TOP] = topBg;
    gPortStereoReliefSink = bottomBg >= 0 ? -lowest : 0;
    sSpritePriority = bottomPriority;
    sLive = TRUE;
}

bool32 Port_Stereo_ReliefLive(void) {
    return sLive;
}

bool32 Port_Stereo_CellHeights(int col, int row, int* bottom, int* top) {
    if (sArea != gRoomControls.area || sRoom != gRoomControls.room || col < 0 || row < 0 || col >= sCols ||
        row >= sRows) {
        return FALSE;
    }
    *bottom = sHeight[row * sCols + col];
    *top = sHeightTop[row * sCols + col];
    return TRUE;
}

bool32 Port_Stereo_RoomView(PortStereoRoomView* view) {
    if (gMain.task != TASK_GAME || gMapBottom.bgSettings == NULL) {
        return FALSE;
    }
    if (sArea != gRoomControls.area || sRoom != gRoomControls.room || sEditsRevision != PortStereoEdits_Revision()) {
        sArea = gRoomControls.area;
        sRoom = gRoomControls.room;
        sAge = 0;
        MeasureRoom();
    }
    *view = (PortStereoRoomView){ sCols,      sRows,       sKind,          sGround,     sHeight,
                                  sHeightTop, sAutoGround, sAutoHeight, sAutoHeightTop, { sTileHash[0], sTileHash[1] } };
    return TRUE;
}
