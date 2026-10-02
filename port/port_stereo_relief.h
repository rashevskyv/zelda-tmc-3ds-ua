/**
 * @file port_stereo_relief.h
 * @brief Stereoscopic 3D relief: how high each cell of a room stands.
 *
 * Pure arithmetic over a room cut into 8x8 cells, kept apart from the engine
 * so it can be tested on a host (platform/3ds/tests/port_stereo_relief_test.c).
 *
 * Two readings of the room are put together.
 *
 * GROUND LEVELS. The game marks every cliff edge Link can hop off, and which
 * way he lands. That is a statement about height: the ground behind a ledge
 * is higher than the ground in front of it. Open ground is flooded into
 * regions that ledges, ramps and walls keep apart, each ledge says which of
 * two regions is the upper one, and a cliff that faces the camera says by how
 * much -- its face is as many cells tall as it is drawn. Starting from the
 * largest region the levels follow. Water and pits lie one unit under the
 * lowest ground they border. A ramp runs from the level at one end to the level at
 * the other.
 *
 * THINGS. The art shows the world from above and in front, so a thing's foot
 * is its southern edge and every row further up the screen is higher up the
 * thing. A solid cell's height is the level of the ground at the foot of its
 * column plus the run of solid cells up to it: a fence one tile deep stays
 * low, a house or a cliff climbs to the cap, and a wall with no ground below
 * it -- the one along the bottom of a room -- is all top. A column answers to
 * the structure it is cut into: the cells over a doorway continue the walls
 * on either side instead of starting again at the lintel.
 *
 * Without the levels every plateau lay as deep as the field below while the
 * ledges around it climbed as walls do, so a yard on a rise looked like a pit
 * inside a rampart.
 *
 * The rules for things are those of the automatic structure detector in the
 * Gen1Recomp voxel mod (water first, then walkable ground, the rest raised to
 * a height measured from the drawing; a column adopts its region). The levels
 * come from data that game does not have.
 */
#ifndef PORT_STEREO_RELIEF_H
#define PORT_STEREO_RELIEF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    PORT_STEREO_CELL_OPEN,    /* walkable: ground */
    PORT_STEREO_CELL_SOLID,   /* blocks movement: part of a thing */
    PORT_STEREO_CELL_OUTSIDE, /* past the room's edge: no ground to stand a foot on */
    PORT_STEREO_CELL_SUNKEN,  /* water, a pit: open, and below the ground */
    PORT_STEREO_CELL_RAMP,    /* stairs, a slope: walkable, between two levels */
    /* A cliff edge to hop off; the name is the side Link lands on, the lower one. */
    PORT_STEREO_CELL_LEDGE_N,
    PORT_STEREO_CELL_LEDGE_E,
    PORT_STEREO_CELL_LEDGE_S,
    PORT_STEREO_CELL_LEDGE_W,

    /* Two cells of drawing are one depth unit of height. */
    PORT_STEREO_RELIEF_CELLS_PER_UNIT = 2,
    /* A gap in a solid row this narrow or narrower is an opening in one thing
     * (a door is two cells wide), not the space between two things. */
    PORT_STEREO_RELIEF_OPENING_CELLS = 4,
    /* A thing rises at most this far above the ground it stands on. */
    PORT_STEREO_RELIEF_THING_UNITS = 4,
    /* How far a ledge is searched across for the ground on either side. */
    PORT_STEREO_RELIEF_LEDGE_REACH = 8,
    /* A drop whose face is not drawn (a ledge seen from the side or from
     * behind) when no drawn face in the room says better. */
    PORT_STEREO_RELIEF_DEFAULT_DROP = 2,
    /* Heights are kept inside this range, in units from the reference ground. */
    PORT_STEREO_RELIEF_MIN = -3,
    PORT_STEREO_RELIEF_MAX = 5,
    PORT_STEREO_RELIEF_MAX_LEVEL = 3,

    PORT_STEREO_RELIEF_MAX_COLS = 256,
    PORT_STEREO_RELIEF_MAX_REGIONS = 512,
    PORT_STEREO_RELIEF_MAX_EDGES = 768,
    PORT_STEREO_RELIEF_UNKNOWN = -128,
    PORT_STEREO_RELIEF_NO_FOOT = 127,
};

/* A room to measure. kind is rows x cols, row 0 at the top. region and queue
 * are scratch of the same size. */
typedef struct PortStereoRoom {
    int cols, rows;
    const uint8_t* kind;
    uint16_t* region;
    uint16_t* queue;
} PortStereoRoom;

typedef struct PortStereoEdge {
    uint16_t high, low; /* regions */
    int8_t drop;        /* units, 0 when the face is not drawn */
} PortStereoEdge;

static const int kPortStereoDx[4] = { 0, 1, 0, -1 }; /* N E S W, in the order of the ledge kinds */
static const int kPortStereoDy[4] = { -1, 0, 1, 0 };

static inline bool PortStereo_IsLedge(uint8_t kind) {
    return kind >= PORT_STEREO_CELL_LEDGE_N && kind <= PORT_STEREO_CELL_LEDGE_W;
}

static inline bool PortStereo_IsGround(uint8_t kind) {
    return kind == PORT_STEREO_CELL_OPEN || kind == PORT_STEREO_CELL_SUNKEN;
}

static inline int PortStereo_Clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static inline int PortStereo_RunUnits(int run) {
    return (run + PORT_STEREO_RELIEF_CELLS_PER_UNIT - 1) / PORT_STEREO_RELIEF_CELLS_PER_UNIT;
}

/* The first ground cell from (col, row) going (dx, dy), across ledge cells
 * only; -1 when something else is in the way or nothing is near. */
static inline int PortStereo_GroundBeyond(const PortStereoRoom* room, int col, int row, int dx, int dy) {
    for (int step = 0; step < PORT_STEREO_RELIEF_LEDGE_REACH; ++step) {
        col += dx;
        row += dy;
        if (col < 0 || row < 0 || col >= room->cols || row >= room->rows) return -1;
        const uint8_t kind = room->kind[row * room->cols + col];
        if (PortStereo_IsGround(kind)) return row * room->cols + col;
        if (!PortStereo_IsLedge(kind)) return -1;
    }
    return -1;
}

/* The ground level of every region, in level[region]; region 0 is "none".
 * Fills room->region. */
static inline void PortStereo_GroundLevels(const PortStereoRoom* room,
                                           int8_t level[PORT_STEREO_RELIEF_MAX_REGIONS]) {
    const int cols = room->cols, rows = room->rows, cells = cols * rows;
    static PortStereoEdge edges[PORT_STEREO_RELIEF_MAX_EDGES];
    int edgeCount = 0;

    /* Regions: ground that can be walked (or swum) across without a hop or a
     * ramp. Water and land never share one. */
    static uint16_t size[PORT_STEREO_RELIEF_MAX_REGIONS]; /* of land regions; 0 for water */
    memset(room->region, 0, (size_t)cells * sizeof(*room->region));
    memset(size, 0, sizeof(size));
    int regionCount = 1, reference = 0, referenceSize = 0;
    for (int start = 0; start < cells; ++start) {
        if (!PortStereo_IsGround(room->kind[start]) || room->region[start] != 0) continue;
        if (regionCount >= PORT_STEREO_RELIEF_MAX_REGIONS) break;
        const uint16_t id = (uint16_t)regionCount++;
        const uint8_t kind = room->kind[start];
        int head = 0, tail = 0, count = 0;
        room->queue[tail++] = (uint16_t)start;
        room->region[start] = id;
        while (head < tail) {
            const int at = room->queue[head++];
            const int col = at % cols, row = at / cols;
            ++count;
            for (int side = 0; side < 4; ++side) {
                const int nc = col + kPortStereoDx[side], nr = row + kPortStereoDy[side];
                if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) continue;
                const int next = nr * cols + nc;
                if (room->kind[next] != kind || room->region[next] != 0) continue;
                room->region[next] = id;
                room->queue[tail++] = (uint16_t)next;
            }
        }
        if (kind == PORT_STEREO_CELL_OPEN) {
            size[id] = (uint16_t)(count > 0xffff ? 0xffff : count);
            if (count > referenceSize) {
                reference = id;
                referenceSize = count;
            }
        }
    }

    /* What the room says about which region is above which. */
    int dropVotes[PORT_STEREO_RELIEF_MAX_LEVEL + 1] = { 0 };
    for (int at = 0; at < cells; ++at) {
        const uint8_t kind = room->kind[at];
        const int col = at % cols, row = at / cols;
        uint16_t high = 0, low = 0;
        int drop = 0;
        if (PortStereo_IsLedge(kind)) {
            const int side = kind - PORT_STEREO_CELL_LEDGE_N;
            const int below = PortStereo_GroundBeyond(room, col, row, kPortStereoDx[side], kPortStereoDy[side]);
            const int above = PortStereo_GroundBeyond(room, col, row, -kPortStereoDx[side], -kPortStereoDy[side]);
            if (below < 0 || above < 0) continue;
            high = room->region[above];
            low = room->region[below];
            if (high == low) continue;
            if (kind == PORT_STEREO_CELL_LEDGE_S && room->kind[below] == PORT_STEREO_CELL_OPEN) {
                /* a cliff facing the camera: its face is drawn, and counts */
                int face = 1;
                for (int r = row - 1; r >= 0 && room->kind[r * cols + col] == kind; --r) ++face;
                for (int r = row + 1; r < rows && room->kind[r * cols + col] == kind; ++r) ++face;
                drop = PortStereo_Clamp(PortStereo_RunUnits(face), 1, PORT_STEREO_RELIEF_MAX_LEVEL);
                ++dropVotes[drop];
            }
        } else if (kind == PORT_STEREO_CELL_SUNKEN) {
            /* water against open ground with no ledge between: a shore */
            for (int side = 0; side < 4 && high == 0; ++side) {
                const int nc = col + kPortStereoDx[side], nr = row + kPortStereoDy[side];
                if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) continue;
                if (room->kind[nr * cols + nc] == PORT_STEREO_CELL_OPEN) high = room->region[nr * cols + nc];
            }
            low = room->region[at];
            drop = 1;
        } else {
            continue;
        }
        if (high == 0 || low == 0 || high == low) continue;
        int found = -1;
        for (int i = 0; i < edgeCount && found < 0; ++i) {
            if (edges[i].high == high && edges[i].low == low) found = i;
        }
        if (found >= 0) {
            if (drop > edges[found].drop) edges[found].drop = (int8_t)drop;
        } else if (edgeCount < PORT_STEREO_RELIEF_MAX_EDGES) {
            edges[edgeCount++] = (PortStereoEdge){ high, low, (int8_t)drop };
        }
    }
    /* An undrawn drop is taken to be what the room's drawn cliffs mostly are. */
    int usual = PORT_STEREO_RELIEF_DEFAULT_DROP, usualVotes = 0;
    for (int drop = 1; drop <= PORT_STEREO_RELIEF_MAX_LEVEL; ++drop) {
        if (dropVotes[drop] > usualVotes) {
            usual = drop;
            usualVotes = dropVotes[drop];
        }
    }

    /* Land first. Levels follow from the reference region outward: each step
     * settles one more region, and takes a drawn drop before an undrawn one,
     * so a guess never overrules a measurement. Land that no ledge ties to
     * the reference -- the far bank of a river, a walled garden -- is taken
     * to be level with it, and what it rules over follows from there. */
    for (int i = 0; i < PORT_STEREO_RELIEF_MAX_REGIONS; ++i) level[i] = PORT_STEREO_RELIEF_UNKNOWN;
    level[0] = 0;
    if (reference != 0) level[reference] = 0;
    for (int settled = 0; settled < 2 * regionCount; ++settled) {
        bool changed = false;
        for (int measured = 1; measured >= 0 && !changed; --measured) {
            for (int i = 0; i < edgeCount && !changed; ++i) {
                const PortStereoEdge* edge = &edges[i];
                if (size[edge->low] == 0 || (edge->drop != 0) != (measured != 0)) continue;
                const int drop = edge->drop != 0 ? edge->drop : usual;
                const bool lowKnown = level[edge->low] != PORT_STEREO_RELIEF_UNKNOWN;
                const bool highKnown = level[edge->high] != PORT_STEREO_RELIEF_UNKNOWN;
                if (lowKnown && !highKnown) {
                    level[edge->high] = (int8_t)(level[edge->low] + drop);
                    changed = true;
                } else if (highKnown && !lowKnown) {
                    level[edge->low] = (int8_t)(level[edge->high] - drop);
                    changed = true;
                }
            }
        }
        if (changed) continue;
        /* Nothing more follows. Start the next group of regions from its
         * largest piece of land. */
        int seed = 0;
        for (int i = 0; i < edgeCount; ++i) {
            if (size[edges[i].low] == 0) continue;
            const uint16_t ends[2] = { edges[i].high, edges[i].low };
            for (int end = 0; end < 2; ++end) {
                const uint16_t id = ends[end];
                if (level[id] != PORT_STEREO_RELIEF_UNKNOWN) continue;
                if (seed == 0 || size[id] > size[seed]) seed = id;
            }
        }
        if (seed == 0) break;
        level[seed] = 0;
    }
    for (int i = 1; i < regionCount; ++i) {
        if (size[i] != 0 && level[i] == PORT_STEREO_RELIEF_UNKNOWN) level[i] = 0;
    }
    /* Then water: one unit under the lowest land it touches, so every bank
     * keeps a lip. A lake between a meadow and a hollow lies under the
     * hollow. */
    for (int i = 0; i < edgeCount; ++i) {
        const PortStereoEdge* edge = &edges[i];
        if (size[edge->low] != 0) continue;
        const int under = level[edge->high] - 1;
        if (level[edge->low] == PORT_STEREO_RELIEF_UNKNOWN || under < level[edge->low]) level[edge->low] = (int8_t)under;
    }
    for (int i = 1; i < regionCount; ++i) {
        if (level[i] == PORT_STEREO_RELIEF_UNKNOWN) level[i] = -1;
        level[i] = (int8_t)PortStereo_Clamp(level[i], PORT_STEREO_RELIEF_MIN, PORT_STEREO_RELIEF_MAX_LEVEL);
    }
}

/* Heights of everything that is not a plain thing: ground, water, ledges that
 * border ground, ramps. Cells left PORT_STEREO_RELIEF_UNKNOWN are things,
 * measured by PortStereo_ThingHeights. */
static inline void PortStereo_GroundHeights(const PortStereoRoom* room, const int8_t* level, int8_t* height) {
    const int cols = room->cols, rows = room->rows, cells = cols * rows;
    for (int at = 0; at < cells; ++at) {
        height[at] = PortStereo_IsGround(room->kind[at]) ? level[room->region[at]] : PORT_STEREO_RELIEF_UNKNOWN;
    }
    for (int at = 0; at < cells; ++at) {
        const uint8_t kind = room->kind[at];
        if (!PortStereo_IsLedge(kind)) continue;
        const int col = at % cols, row = at / cols;
        const int side = kind - PORT_STEREO_CELL_LEDGE_N;
        if (kind == PORT_STEREO_CELL_LEDGE_S) {
            /* a face: it climbs from the ground at its foot */
            const int below = PortStereo_GroundBeyond(room, col, row, 0, 1);
            if (below < 0) continue;
            int fromFoot = 1;
            for (int r = row + 1; r < rows && room->kind[r * cols + col] == kind; ++r) ++fromFoot;
            height[at] = (int8_t)(height[below] + PortStereo_RunUnits(fromFoot));
            continue;
        }
        /* Seen from the side or from behind only the rim shows: it is level
         * with the ground it edges -- the ground across it, or at a corner
         * the ground it touches. */
        int rim = PORT_STEREO_RELIEF_UNKNOWN;
        const int above = PortStereo_GroundBeyond(room, col, row, -kPortStereoDx[side], -kPortStereoDy[side]);
        if (above >= 0 && room->kind[above] == PORT_STEREO_CELL_OPEN) rim = height[above];
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nc = col + dx, nr = row + dy;
                if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) continue;
                const int near = nr * cols + nc;
                if (room->kind[near] == PORT_STEREO_CELL_OPEN && (rim == PORT_STEREO_RELIEF_UNKNOWN || height[near] > rim))
                    rim = height[near];
            }
        }
        height[at] = (int8_t)rim; /* unknown: a rim against a wall, part of the wall */
    }
    /* Ramps: a run down the screen goes evenly from the level at its top to
     * the level at its bottom; one with a single known end is flat at it. */
    for (int col = 0; col < cols; ++col) {
        for (int row = 0; row < rows; ++row) {
            if (room->kind[row * cols + col] != PORT_STEREO_CELL_RAMP) continue;
            int last = row;
            while (last + 1 < rows && room->kind[(last + 1) * cols + col] == PORT_STEREO_CELL_RAMP) ++last;
            const int top = row > 0 ? height[(row - 1) * cols + col] : PORT_STEREO_RELIEF_UNKNOWN;
            const int bottom = last + 1 < rows ? height[(last + 1) * cols + col] : PORT_STEREO_RELIEF_UNKNOWN;
            const int count = last - row + 1;
            for (int r = row; r <= last; ++r) {
                int value = 0;
                if (top != PORT_STEREO_RELIEF_UNKNOWN && bottom != PORT_STEREO_RELIEF_UNKNOWN) {
                    /* rounded to the nearer end, so the ramp meets both */
                    const int twice = 2 * (top * (last - r + 1) + bottom * (r - row + 1));
                    const int span = 2 * (count + 1);
                    value = (twice + (twice >= 0 ? span / 2 : -(span / 2))) / span;
                } else if (top != PORT_STEREO_RELIEF_UNKNOWN) {
                    value = top;
                } else if (bottom != PORT_STEREO_RELIEF_UNKNOWN) {
                    value = bottom;
                } else {
                    /* a ramp across the screen: level with the ground beside it */
                    const int left = col > 0 ? height[r * cols + col - 1] : PORT_STEREO_RELIEF_UNKNOWN;
                    const int right = col + 1 < cols ? height[r * cols + col + 1] : PORT_STEREO_RELIEF_UNKNOWN;
                    value = left != PORT_STEREO_RELIEF_UNKNOWN ? left : right != PORT_STEREO_RELIEF_UNKNOWN ? right : 0;
                }
                height[r * cols + col] = (int8_t)value;
            }
            row = last;
        }
    }
}

/* Heights of the things: every cell `ground` left unknown, plus every cell
 * `alsoSolid` marks (may be NULL) -- a second layer's own solid cells, which
 * stand on the same ground. Writes all of height. */
static inline void PortStereo_ThingHeights(const PortStereoRoom* room, const int8_t* ground, const uint8_t* alsoSolid,
                                           int8_t* height) {
    const int cols = room->cols, rows = room->rows;
    /* per column, as of the row below: the run of thing cells and the level
     * of the ground at its foot (NO_FOOT: it stands on nothing in the room) */
    uint8_t run[PORT_STEREO_RELIEF_MAX_COLS], belowRun[PORT_STEREO_RELIEF_MAX_COLS];
    int8_t foot[PORT_STEREO_RELIEF_MAX_COLS], belowFoot[PORT_STEREO_RELIEF_MAX_COLS];
    if (cols > PORT_STEREO_RELIEF_MAX_COLS) return;
    const int capRun = PORT_STEREO_RELIEF_THING_UNITS * PORT_STEREO_RELIEF_CELLS_PER_UNIT;
    for (int col = 0; col < cols; ++col) {
        run[col] = 0;
        foot[col] = PORT_STEREO_RELIEF_NO_FOOT; /* below the room: unknown, so not a foot */
    }
    for (int row = rows - 1; row >= 0; --row) {
        memcpy(belowRun, run, (size_t)cols);
        memcpy(belowFoot, foot, (size_t)cols);
        for (int col = 0; col < cols; ++col) {
            const int at = row * cols + col;
            const uint8_t kind = room->kind[at];
            if (kind == PORT_STEREO_CELL_OUTSIDE) {
                height[at] = 0;
                run[col] = 0;
                foot[col] = PORT_STEREO_RELIEF_NO_FOOT;
                continue;
            }
            if (ground[at] != PORT_STEREO_RELIEF_UNKNOWN && !(alsoSolid && alsoSolid[at])) {
                /* ground: whatever stands on it starts here */
                height[at] = ground[at];
                run[col] = 0;
                foot[col] = ground[at];
                continue;
            }
            int cellRun = belowRun[col], cellFoot = belowFoot[col];
            if (cellRun == 0 && cellFoot != PORT_STEREO_RELIEF_NO_FOOT && row + 1 < rows) {
                /* A foot. Over a narrow opening the column answers to the
                 * structure it is cut into: it continues the lower of the two
                 * walls flanking the opening. */
                int left = col, right = col;
                while (left >= 0 && belowRun[left] == 0 && belowFoot[left] != PORT_STEREO_RELIEF_NO_FOOT) --left;
                while (right < cols && belowRun[right] == 0 && belowFoot[right] != PORT_STEREO_RELIEF_NO_FOOT)
                    ++right;
                if (left >= 0 && right < cols && right - left - 1 <= PORT_STEREO_RELIEF_OPENING_CELLS &&
                    belowRun[left] != 0 && belowRun[right] != 0) {
                    const int jamb =
                        height[(row + 1) * cols + left] <= height[(row + 1) * cols + right] ? left : right;
                    cellRun = belowRun[jamb];
                    cellFoot = belowFoot[jamb];
                }
            }
            cellRun = cellRun < capRun ? cellRun + 1 : capRun;
            run[col] = (uint8_t)cellRun;
            foot[col] = (int8_t)cellFoot;
            height[at] = cellFoot == PORT_STEREO_RELIEF_NO_FOOT
                             ? (int8_t)PORT_STEREO_RELIEF_THING_UNITS
                             : (int8_t)PortStereo_Clamp(cellFoot + PortStereo_RunUnits(cellRun),
                                                        PORT_STEREO_RELIEF_MIN, PORT_STEREO_RELIEF_MAX);
        }
    }
}

/* Everything at once. height receives each cell's height in units from the
 * reference ground; heightAbove (may be NULL) the same for a second layer
 * whose own solid cells are alsoSolid. ground is scratch, rows x cols, and
 * comes back holding the ground heights with things left unknown. */
static inline void PortStereo_RoomHeights(const PortStereoRoom* room, const uint8_t* alsoSolid, int8_t* ground,
                                          int8_t* height, int8_t* heightAbove) {
    static int8_t level[PORT_STEREO_RELIEF_MAX_REGIONS];
    PortStereo_GroundLevels(room, level);
    PortStereo_GroundHeights(room, level, ground);
    PortStereo_ThingHeights(room, ground, NULL, height);
    if (heightAbove) PortStereo_ThingHeights(room, ground, alsoSolid, heightAbove);
}

#endif /* PORT_STEREO_RELIEF_H */
