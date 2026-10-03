/**
 * @file port_stereo.h
 * @brief Stereoscopic 3D: a depth of its own for chosen sprites.
 *
 * A renderer that draws the layers apart for two eyes places every sprite by
 * its GBA priority. That is wrong wherever the art means something else --
 * the title logo, its subtitle and PRESS START share two priorities but stand
 * at four depths. Game code can name the depth instead; renderers that draw
 * one flat image ignore it.
 *
 * Depth is in the renderer's units, 0 on the screen plane and larger deeper:
 * backgrounds of priority 1, 2, 3 sit at 3, 6, 9. Down to
 * -MODE1_STEREO_DEPTH_NEAR stands in front of the screen.
 */
#ifndef PORT_STEREO_H
#define PORT_STEREO_H

#include "cpu/mode1.h"
#include "entity.h"

/* A depth as stored per OAM entry; 0 means "by priority". */
#define PORT_STEREO_DEPTH(units) ((u8)((units) + 1 + MODE1_STEREO_DEPTH_NEAR))

/* Depth for a whole background (0..3), or 0 for "by priority" again. For
 * screens whose only layer is the picture itself -- a logo on a plain
 * backdrop would otherwise sit as deep as a room's floor. */
void Port_Stereo_SetBgDepth(unsigned bg, u8 depth);
/* Depth for the sprites DrawDirect emits; set around the call, then back to 0. */
extern u8 gPortStereoDirectDepth;
/* Depth for an entity's sprites, or NULL to place every entity by priority. */
extern u8 (*gPortStereoEntityDepth)(const Entity* entity);
/* The same, asked second, for where the entity stands: the relief's answer
 * for sprites on ground that is not at the reference level. */
extern u8 (*gPortStereoEntityGround)(const Entity* entity);
/* Depth for an entity's shadow -- the ground it stands on, whatever the
 * entity's own depth -- or 0 for "a unit behind the entity". */
extern u8 (*gPortStereoEntityShadow)(const Entity* entity);

/* Relief: which 8x8 cells of the room's two map backgrounds stand above the
 * floor, as depth units per cell, indexed [row * COLS + col] by the cell's
 * unwrapped tilemap position. Refreshed once per frame together with the
 * tilemaps it describes; gPortStereoReliefBg is each layer's background
 * number, or -1 while its grid does not apply. */
enum {
    PORT_STEREO_RELIEF_COLS = 40,
    PORT_STEREO_RELIEF_ROWS = 32,
    /* The largest value a grid cell can hold: from the lowest ground a room
     * can have (three units under the reference) to the tallest thing (five
     * over it); see port_stereo_relief.h. */
    PORT_STEREO_RELIEF_MAX_CELL = 8,
};
enum { PORT_STEREO_RELIEF_BOTTOM, PORT_STEREO_RELIEF_TOP, PORT_STEREO_RELIEF_LAYERS };
extern u8 gPortStereoRelief[PORT_STEREO_RELIEF_LAYERS][PORT_STEREO_RELIEF_ROWS * PORT_STEREO_RELIEF_COLS];
extern int gPortStereoReliefBg[PORT_STEREO_RELIEF_LAYERS];
/* Depth units the bottom layer must be drawn deeper by whenever its grid is
 * used: its cells are measured from the bottom of the water, not the ground. */
extern int gPortStereoReliefSink;
void Port_Stereo_CommitRelief(void);
/* Whether this frame's relief is drawn (3D slider up, relief on, in a room). */
bool32 Port_Stereo_ReliefLive(void);
/* A cell of the current room as measured and edited, in units above the
 * reference ground, per map layer; false when the room is not measured. */
bool32 Port_Stereo_CellHeights(int col, int row, int* bottom, int* top);

#endif /* PORT_STEREO_H */
