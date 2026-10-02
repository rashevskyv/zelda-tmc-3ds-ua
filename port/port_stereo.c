/**
 * @file port_stereo.c
 * @brief Stereoscopic 3D relief: which cells of the room stand above the floor.
 *
 * A room is two flat backgrounds, so a renderer that draws layers apart for
 * two eyes shows walls, trees and furniture lying in the floor. The game does know which cells are solid: MapLayer.collisionData
 * holds, for every 16x16 tile, which of its four 8x8 quarters block movement.
 * Solid is not the same as tall -- a fence and a cliff both block -- but it
 * separates "things" from "ground" in every room with no authored data.
 *
 * Each frame this turns the visible part of those maps into a grid over each
 * background's own 8x8 tilemap cells, which is the unit the renderer draws.
 */
#include "port_stereo.h"
#include "port_stereo_relief.h"

#include "global.h"
#include "main.h"
#include "map.h"
#include "room.h"
#include "screen.h"

u8 gPortStereoRelief[PORT_STEREO_RELIEF_LAYERS][PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
int gPortStereoReliefBg[PORT_STEREO_RELIEF_LAYERS] = { -1, -1 };
int gPortStereoReliefSink;

/* Whether one 8x8 quarter of a tile blocks movement, from the tile's collision
 * value (see IsTileCollision in src/movement.c). 1..15 is a mask of the four
 * quarters; larger values are shaped tiles, solid for all practical purposes
 * except the three that never block. */
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
    return collision != 0x22 && collision != 0x28 && collision != 0x2a;
}

/* Whether a tile's behaviour puts it below the ground: deep water, a pit, a
 * hole (the ActTile values that lead to those surfaces, see include/tiles.h).
 * Shallow water is ground with a film on it and stays level. */
static bool32 TileIsSunken(u32 actTile) {
    switch (actTile) {
        case 0x0d: /* -> SURFACE_PIT */
        case 0x10: /* -> SURFACE_WATER */
        case 0x19: /* -> SURFACE_HOLE */
        case 0xf0: /* -> SURFACE_HOLE */
            return TRUE;
        default:
            return FALSE;
    }
}

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

void Port_Stereo_CommitRelief(void) {
    /* Only while the camera follows inside one room. A room-to-room scroll
     * streams two rooms through the tilemap, and menus and cutscenes point the
     * layers elsewhere; the tilemaps then no longer mirror these collision
     * maps. */
    gPortStereoReliefBg[PORT_STEREO_RELIEF_BOTTOM] = -1;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_TOP] = -1;
    gPortStereoReliefSink = 0;
    if (gMain.task != TASK_GAME || gRoomControls.scrollAction > 1) {
        return;
    }
    const int bottomBg = MapBackground(&gMapBottom);
    const int topBg = MapBackground(&gMapTop);
    if (bottomBg < 0 && topBg < 0) {
        return;
    }

    /* The engine refills the tilemaps every 16 pixels of camera travel so
     * that cell (0, 1) is the tile the camera's top-left corner is in; in
     * between only the scroll registers move (UpdateScreenShake in
     * src/scroll.c). */
    const s32 baseX = (gRoomControls.scroll_x - gRoomControls.origin_x) & ~0xf;
    const s32 baseY = ((gRoomControls.scroll_y - gRoomControls.origin_y) & ~0xf) - 8;
    const s32 roomWidth = gRoomControls.width;
    const s32 roomHeight = gRoomControls.height;
    /* Sort the cells of each layer into ground, thing and off-the-room, then
     * read the heights off that (port_stereo_relief.h). */
    static u8 kindBottom[PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
    static u8 kindTop[PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
    u8* bottom = kindBottom;
    u8* top = kindTop;
    for (s32 row = 0; row < PORT_STEREO_RELIEF_ROWS; ++row) {
        const s32 y = baseY + row * 8;
        for (s32 col = 0; col < PORT_STEREO_RELIEF_COLS; ++col, ++bottom, ++top) {
            const s32 x = baseX + col * 8;
            if (x < 0 || y < 0 || x >= roomWidth || y >= roomHeight || x >= 64 * 16 || y >= 64 * 16) {
                *bottom = *top = PORT_STEREO_CELL_OUTSIDE;
                continue;
            }
            const u32 tile = (u32)(x >> 4) | ((u32)(y >> 4) << 6);
            const bool32 solidBelow = QuarterIsSolid(gMapBottom.collisionData[tile], (u32)x, (u32)y);
            /* The top layer holds the upper parts of what stands on the bottom
             * one -- furniture, wall tops -- so it rises over a solid cell of
             * either map and stays with the thing it belongs to. */
            const bool32 solidAbove = (solidBelow && !TileIsSunken(gMapBottom.actTiles[tile])) ||
                                      QuarterIsSolid(gMapTop.collisionData[tile], (u32)x, (u32)y);
            /* Deep water blocks movement like a wall, so what a tile IS has to
             * be asked before whether it blocks. */
            *bottom = TileIsSunken(gMapBottom.actTiles[tile]) ? PORT_STEREO_CELL_SUNKEN
                      : solidBelow                            ? PORT_STEREO_CELL_SOLID
                                                              : PORT_STEREO_CELL_OPEN;
            *top = solidAbove ? PORT_STEREO_CELL_SOLID : PORT_STEREO_CELL_OPEN;
        }
    }
    const int sink =
        PortStereo_ReliefHeights(kindBottom, PORT_STEREO_RELIEF_COLS, PORT_STEREO_RELIEF_ROWS,
                                 PORT_STEREO_RELIEF_UNITS, gPortStereoRelief[PORT_STEREO_RELIEF_BOTTOM]);
    /* Water or a pit is in reach of the view: whoever draws this grid must
     * also draw the bottom layer that much deeper than its priority says.
     * Every cell but the sunken ones came back that much taller, so the
     * ground does not move. */
    gPortStereoReliefSink = bottomBg >= 0 ? sink : 0;
    PortStereo_ReliefHeights(kindTop, PORT_STEREO_RELIEF_COLS, PORT_STEREO_RELIEF_ROWS, PORT_STEREO_RELIEF_UNITS,
                             gPortStereoRelief[PORT_STEREO_RELIEF_TOP]);

    /* Heights are measured from the floor, but the renderer raises a cell
     * from its own layer, and outdoors the top layer already stands a whole
     * priority nearer than the floor. Taking that head start off keeps a tree
     * top as high above the ground as its trunk says, instead of stacking the
     * two and pushing every canopy up to the screen plane. */
    if (bottomBg >= 0 && topBg >= 0) {
        const s32 headStart =
            3 * ((s32)(gMapBottom.bgSettings->control & 3) - (s32)(gMapTop.bgSettings->control & 3));
        if (headStart > 0) {
            u8* cell = gPortStereoRelief[PORT_STEREO_RELIEF_TOP];
            for (u32 i = 0; i < PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS; ++i) {
                cell[i] = cell[i] > headStart ? (u8)(cell[i] - headStart) : 0;
            }
        }
    }
    gPortStereoReliefBg[PORT_STEREO_RELIEF_BOTTOM] = bottomBg;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_TOP] = topBg;
}
