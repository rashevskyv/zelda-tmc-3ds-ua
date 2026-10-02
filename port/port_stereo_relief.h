/**
 * @file port_stereo_relief.h
 * @brief Stereoscopic 3D relief: how tall each cell of a room stands.
 *
 * Pure arithmetic over a grid of 8x8 cells, kept apart from the engine so it
 * can be tested on a host (platform/3ds/tests/port_stereo_relief_test.c).
 *
 * The art shows the world from above and in front, so a thing's foot is its
 * southern edge and every row further up the screen is higher up the thing.
 * A cell's height is therefore read off the drawing: the run of solid cells
 * below it before open ground. A fence one tile deep stays low, a house or a
 * cliff climbs to the cap, and a wall with no ground below it -- the one along
 * the bottom of a room -- is all top.
 *
 * The same reading as the automatic structure detector of the Gen1Recomp
 * voxel mod (walkable is ground, the rest rises to a height measured from the
 * drawing), with its region rule: a column answers to the structure it stands
 * in. The column over a doorway has open ground right under it and would read
 * as a low lintel in a tall front; it takes the height of the walls on either
 * side instead.
 *
 * Water and pits lie below the ground, and the mod's rule for them carries
 * over too: they recess, so a shore shows a lip. A renderer that can only
 * bring cells nearer does that by standing everything else one unit up from
 * a layer drawn one unit deeper -- the ground stays where it was and only the
 * water sinks -- so when a grid holds sunken cells every other cell reports
 * one unit more, and the caller is told to sink the layer.
 */
#ifndef PORT_STEREO_RELIEF_H
#define PORT_STEREO_RELIEF_H

#include <stdint.h>

enum {
    PORT_STEREO_CELL_OPEN,    /* walkable: ground */
    PORT_STEREO_CELL_SOLID,   /* blocks movement: part of a thing */
    PORT_STEREO_CELL_OUTSIDE, /* past the room's edge: no ground to stand a foot on */
    PORT_STEREO_CELL_SUNKEN,  /* water, a pit: open, and below the ground */
    /* Two cells of drawing are one depth unit of height. */
    PORT_STEREO_RELIEF_CELLS_PER_UNIT = 2,
    /* A gap in a solid row this narrow or narrower is an opening in one thing
     * (a door is two cells wide), not the space between two things. */
    PORT_STEREO_RELIEF_OPENING_CELLS = 4,
};

/* kind and out are rows x cols, row 0 at the top. out receives each cell's
 * height in depth units: 0 for anything that is not solid, at most capUnits.
 * Returns the units the layer must be sunk by -- 0, or 1 when the grid holds
 * sunken cells; every cell that is not sunken then reports one unit more, so
 * open ground is 1 and the tallest thing capUnits + 1. */
static inline int PortStereo_ReliefHeights(const uint8_t* kind, int cols, int rows, int capUnits, uint8_t* out) {
    const int capRun = capUnits * PORT_STEREO_RELIEF_CELLS_PER_UNIT;

    /* Each column's run of solid cells up from its foot, bottom row first.
     * out holds the run for now. */
    for (int col = 0; col < cols; ++col) {
        int run = capRun; /* below the grid: unknown, so not a foot */
        for (int row = rows - 1; row >= 0; --row) {
            const uint8_t cell = kind[row * cols + col];
            if (cell == PORT_STEREO_CELL_SOLID) {
                run = run < capRun ? run + 1 : capRun;
                out[row * cols + col] = (uint8_t)run;
            } else {
                /* water is as good a place for a foot as ground */
                run = cell == PORT_STEREO_CELL_OUTSIDE ? capRun : 0;
                out[row * cols + col] = 0;
            }
        }
    }

    /* The region rule. A solid cell over a narrow opening takes the lower of
     * the two runs flanking the opening, when that is more than its own. Rows
     * are visited bottom-up so a filled cell carries the new run on up its
     * column, the way the column would have counted without the opening. */
    for (int row = rows - 2; row >= 0; --row) {
        for (int col = 0; col < cols; ++col) {
            const int at = row * cols + col;
            if (kind[at] != PORT_STEREO_CELL_SOLID) continue;
            const uint8_t below = kind[at + cols];
            if (below == PORT_STEREO_CELL_SOLID) {
                /* carry a run raised further down this column */
                const int carried = out[at + cols] < capRun ? out[at + cols] + 1 : capRun;
                if (carried > out[at]) out[at] = (uint8_t)carried;
                continue;
            }
            if (below != PORT_STEREO_CELL_OPEN && below != PORT_STEREO_CELL_SUNKEN) continue;
            /* this cell is a foot: is the open cell under it a narrow opening? */
            int left = col, right = col;
            while (left >= 0 && (kind[(row + 1) * cols + left] == PORT_STEREO_CELL_OPEN ||
                                 kind[(row + 1) * cols + left] == PORT_STEREO_CELL_SUNKEN))
                --left;
            while (right < cols && (kind[(row + 1) * cols + right] == PORT_STEREO_CELL_OPEN ||
                                    kind[(row + 1) * cols + right] == PORT_STEREO_CELL_SUNKEN))
                ++right;
            if (left < 0 || right >= cols || right - left - 1 > PORT_STEREO_RELIEF_OPENING_CELLS) continue;
            if (kind[(row + 1) * cols + left] != PORT_STEREO_CELL_SOLID ||
                kind[(row + 1) * cols + right] != PORT_STEREO_CELL_SOLID)
                continue;
            /* the jambs' runs one row down, plus this row */
            int jamb = out[(row + 1) * cols + left];
            if (out[(row + 1) * cols + right] < jamb) jamb = out[(row + 1) * cols + right];
            jamb = jamb < capRun ? jamb + 1 : capRun;
            if (jamb > out[at]) out[at] = (uint8_t)jamb;
        }
    }

    int sink = 0;
    for (int i = 0; i < rows * cols && !sink; ++i) {
        sink = kind[i] == PORT_STEREO_CELL_SUNKEN;
    }
    for (int i = 0; i < rows * cols; ++i) {
        out[i] = (uint8_t)((out[i] + PORT_STEREO_RELIEF_CELLS_PER_UNIT - 1) / PORT_STEREO_RELIEF_CELLS_PER_UNIT);
        if (sink && kind[i] != PORT_STEREO_CELL_SUNKEN && kind[i] != PORT_STEREO_CELL_OUTSIDE) ++out[i];
    }
    return sink;
}

#endif /* PORT_STEREO_RELIEF_H */
