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

#include "global.h"
#include "main.h"
#include "map.h"
#include "room.h"
#include "screen.h"

u8 gPortStereoRelief[PORT_STEREO_RELIEF_LAYERS][PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
int gPortStereoReliefBg[PORT_STEREO_RELIEF_LAYERS] = { -1, -1 };

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

/* Solid cells counted below a cell stop mattering here: two cells make one
 * unit of height. */
#define RELIEF_RUN_CAP (2 * PORT_STEREO_RELIEF_UNITS)

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
    /* How tall a solid cell stands is read off the drawing, the way the eye
     * reads it: the art shows things from above and in front, so a thing's
     * foot is its southern edge and every row further up the screen is higher
     * up the thing. A cell's height is therefore how many solid cells lie
     * below it before open ground -- a fence one tile deep stays low, a house
     * or a cliff climbs to the cap, and the run of wall along the bottom of a
     * room, with no ground below it, is all top.
     *
     * Walking the grid bottom row first makes that one pass. One unit per 16
     * pixels of drawing keeps the rows a sprite standing in front of a wall
     * can overlap no nearer than the sprite itself. */
    u8 runBottom[PORT_STEREO_RELIEF_COLS];
    u8 runTop[PORT_STEREO_RELIEF_COLS];
    for (s32 col = 0; col < PORT_STEREO_RELIEF_COLS; ++col) {
        runBottom[col] = runTop[col] = RELIEF_RUN_CAP;
    }
    for (s32 row = PORT_STEREO_RELIEF_ROWS - 1; row >= 0; --row) {
        const s32 y = baseY + row * 8;
        u8* bottom = gPortStereoRelief[PORT_STEREO_RELIEF_BOTTOM] + row * PORT_STEREO_RELIEF_COLS;
        u8* top = gPortStereoRelief[PORT_STEREO_RELIEF_TOP] + row * PORT_STEREO_RELIEF_COLS;
        for (s32 col = 0; col < PORT_STEREO_RELIEF_COLS; ++col) {
            const s32 x = baseX + col * 8;
            bottom[col] = top[col] = 0;
            if (x < 0 || x >= roomWidth || x >= 64 * 16) {
                continue;
            }
            if (y < 0 || y >= roomHeight || y >= 64 * 16) {
                /* Past the room's edge there is no ground to stand a foot on. */
                runBottom[col] = runTop[col] = RELIEF_RUN_CAP;
                continue;
            }
            const u32 tile = (u32)(x >> 4) | ((u32)(y >> 4) << 6);
            const bool32 solidBelow = QuarterIsSolid(gMapBottom.collisionData[tile], (u32)x, (u32)y);
            /* The top layer holds the upper parts of what stands on the bottom
             * one -- furniture, wall tops -- so it rises over a solid cell of
             * either map and stays with the thing it belongs to. */
            const bool32 solidAbove = solidBelow || QuarterIsSolid(gMapTop.collisionData[tile], (u32)x, (u32)y);
            runBottom[col] = solidBelow ? (u8)(runBottom[col] < RELIEF_RUN_CAP ? runBottom[col] + 1 : RELIEF_RUN_CAP) : 0;
            runTop[col] = solidAbove ? (u8)(runTop[col] < RELIEF_RUN_CAP ? runTop[col] + 1 : RELIEF_RUN_CAP) : 0;
            bottom[col] = (u8)((runBottom[col] + 1) / 2);
            top[col] = (u8)((runTop[col] + 1) / 2);
        }
    }
    gPortStereoReliefBg[PORT_STEREO_RELIEF_BOTTOM] = bottomBg;
    gPortStereoReliefBg[PORT_STEREO_RELIEF_TOP] = topBg;
}
