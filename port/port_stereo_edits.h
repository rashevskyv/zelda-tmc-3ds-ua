/**
 * @file port_stereo_edits.h
 * @brief Stereoscopic 3D: hand-made corrections to the measured relief.
 *
 * The relief is read off the game's collision and tile behaviour, and that is
 * wrong in places. These corrections are made in the 3D editor (developer
 * tools) and kept in stereo_edits.txt next to the ROM:
 *
 * - per 8x8 cell of a room and per map layer (bottom, top), either a number
 *   of depth units added to what the measurement says or a height that
 *   replaces it; what stands on a raised cell rises with it;
 * - per entity -- one in a given room, near where it was picked, or every
 *   entity of its kind, id and type -- units to bring it nearer.
 *
 * Heights are depth units above the room's reference ground; positive is
 * nearer the viewer.
 */
#ifndef PORT_STEREO_EDITS_H
#define PORT_STEREO_EDITS_H

#include "global.h"

enum {
    PORT_STEREO_EDIT_BOTTOM = 1,
    PORT_STEREO_EDIT_TOP = 2,
    PORT_STEREO_EDIT_BOTH = 3,
};

typedef struct PortStereoEntityKey {
    u8 kind, id, type;
    /* Every entity of this kind, id and type, wherever it is. */
    bool8 all;
    /* Otherwise the one in this room, within a couple of cells of here. */
    u8 area, room;
    u8 col, row;
} PortStereoEntityKey;

/* Reads stereo_edits.txt once; later calls do nothing. */
void PortStereoEdits_Load(void);
/* Writes the edits back if they changed since the last load or save. */
bool32 PortStereoEdits_Save(void);
/* Changes with every edit, so a measured room knows it is stale. */
u32 PortStereoEdits_Revision(void);

/* Applies the room's cell edits to its measured grids (cols x rows,
 * row-major). A replaced bottom height is ground from then on. */
void PortStereoEdits_ApplyRoom(int area, int room, int cols, int rows, s8* height, s8* heightTop, s8* ground,
                               s8 unknownGround);
/* Which of the asked layers of a cell carry an edit (PORT_STEREO_EDIT_*). */
int PortStereoEdits_CellEdited(int area, int room, int col, int row, int layers);

/* On the asked layers of a rectangle of cells: raise by `step` units, drop
 * the edits, or replace the height by `height`. */
void PortStereoEdits_Step(int area, int room, int layers, int col0, int row0, int col1, int row1, int step);
void PortStereoEdits_Reset(int area, int room, int layers, int col0, int row0, int col1, int row1);
void PortStereoEdits_Set(int area, int room, int layers, int col0, int row0, int col1, int row1, int height);

/* The correction for an entity standing at room cell (col, row). */
int PortStereoEdits_EntityDelta(int area, int room, u8 kind, u8 id, u8 type, int col, int row);
/* The edit for exactly this key: its delta, or 0. */
int PortStereoEdits_KeyDelta(const PortStereoEntityKey* key);
int PortStereoEdits_AdjustEntity(const PortStereoEntityKey* key, int step, bool32 clear);

#endif /* PORT_STEREO_EDITS_H */
