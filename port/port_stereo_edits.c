/**
 * @file port_stereo_edits.c
 * @brief Stereoscopic 3D: hand-made corrections to the measured relief.
 *
 * See port_stereo_edits.h. A room with edits gets a full 128x128 grid of
 * them, allocated on its first edit; the file is plain text, one run of equal
 * cells of a row per line, so it can be read, diffed and folded into the code
 * later:
 *
 *   cell <area> <room> <b|t> <d|s> <row> <col0> <col1> <value>
 *   ent <kind> <id> <type> all <delta>
 *   ent <kind> <id> <type> <area> <room> <col> <row> <delta>
 *
 * b/t is the bottom or top map layer, d adds the value to the measured height
 * and s replaces it. Area, room, kind, id and type are hex, as the developer
 * overlay shows them; the rest decimal. The first editor build wrote
 * rectangles instead (`rect <area> <room> <layers> <col0> <row0> <col1> <row1>
 * <delta>`); those are still read, as additions.
 */
#include "port_stereo_edits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEREO_EDITS_FILE "stereo_edits.txt"

enum {
    SIDE = 128,
    MAX_ROOMS = 96,
    MAX_ENTITIES = 256,
    ENTITY_REACH = 2,
    DELTA_LIMIT = 12,
    HEIGHT_LIMIT = 24,
};

/* Per layer: whether the cell is edited, and whether its value replaces the
 * measured height rather than adding to it. */
enum { CELL_HAS = 1, CELL_SET = 2 };

typedef struct {
    u8 area, room;
    u8 flags[2][SIDE * SIDE];
    s8 value[2][SIDE * SIDE];
} RoomEdits;

typedef struct {
    PortStereoEntityKey key;
    s8 delta;
} EntityEdit;

static RoomEdits* sRooms[MAX_ROOMS];
static int sRoomCount;
static EntityEdit sEntities[MAX_ENTITIES];
static int sEntityCount;
static bool32 sLoaded;
static bool32 sDirty;
static u32 sRevision = 1;

static int Clamp(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static RoomEdits* FindRoom(int area, int room, bool32 create) {
    for (int i = 0; i < sRoomCount; ++i) {
        if (sRooms[i]->area == area && sRooms[i]->room == room) {
            return sRooms[i];
        }
    }
    if (!create || sRoomCount >= MAX_ROOMS) {
        return NULL;
    }
    RoomEdits* edits = calloc(1, sizeof(RoomEdits));
    if (edits == NULL) {
        return NULL;
    }
    edits->area = (u8)area;
    edits->room = (u8)room;
    sRooms[sRoomCount++] = edits;
    return edits;
}

static bool32 ClipRect(int* col0, int* row0, int* col1, int* row1) {
    *col0 = Clamp(*col0, 0, SIDE - 1);
    *row0 = Clamp(*row0, 0, SIDE - 1);
    *col1 = Clamp(*col1, 0, SIDE - 1);
    *row1 = Clamp(*row1, 0, SIDE - 1);
    return *col0 <= *col1 && *row0 <= *row1;
}

static void Changed(void) {
    sDirty = TRUE;
    ++sRevision;
}

static void StepCells(RoomEdits* edits, int layers, int col0, int row0, int col1, int row1, int step) {
    for (int layer = 0; layer < 2; ++layer) {
        if (!(layers & (1 << layer))) {
            continue;
        }
        for (int row = row0; row <= row1; ++row) {
            for (int col = col0; col <= col1; ++col) {
                u8* flags = &edits->flags[layer][row * SIDE + col];
                s8* value = &edits->value[layer][row * SIDE + col];
                const int limit = (*flags & CELL_SET) ? HEIGHT_LIMIT : DELTA_LIMIT;
                *value = (s8)Clamp((*flags & CELL_HAS ? *value : 0) + step, -limit, limit);
                *flags = (*value != 0 || (*flags & CELL_SET)) ? (u8)(*flags | CELL_HAS) : 0;
            }
        }
    }
}

static void SetCells(RoomEdits* edits, int layer, int row, int col0, int col1, bool32 set, int value) {
    for (int col = col0; col <= col1; ++col) {
        edits->flags[layer][row * SIDE + col] = (u8)(CELL_HAS | (set ? CELL_SET : 0));
        edits->value[layer][row * SIDE + col] = (s8)value;
    }
}

void PortStereoEdits_Load(void) {
    if (sLoaded) {
        return;
    }
    sLoaded = TRUE;
    FILE* file = fopen(STEREO_EDITS_FILE, "r");
    if (file == NULL) {
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned a, b, c, d, e, f, g;
        char layer, mode;
        int value;
        if (sscanf(line, "cell %x %x %c %c %u %u %u %d", &a, &b, &layer, &mode, &c, &d, &e, &value) == 8) {
            int col0 = (int)d, row0 = (int)c, col1 = (int)e, row1 = (int)c;
            RoomEdits* edits = FindRoom((int)a, (int)b, TRUE);
            if (edits != NULL && (layer == 'b' || layer == 't') && (mode == 'd' || mode == 's') &&
                ClipRect(&col0, &row0, &col1, &row1)) {
                SetCells(edits, layer == 't', row0, col0, col1, mode == 's',
                         Clamp(value, -HEIGHT_LIMIT, HEIGHT_LIMIT));
            }
        } else if (sscanf(line, "rect %x %x %u %u %u %u %u %d", &a, &b, &c, &d, &e, &f, &g, &value) == 8) {
            int col0 = (int)d, row0 = (int)e, col1 = (int)f, row1 = (int)g;
            RoomEdits* edits = FindRoom((int)a, (int)b, TRUE);
            if (edits != NULL && ClipRect(&col0, &row0, &col1, &row1)) {
                StepCells(edits, (int)c & PORT_STEREO_EDIT_BOTH, col0, row0, col1, row1, value);
            }
        } else if (sscanf(line, "ent %x %x %x all %d", &a, &b, &c, &value) == 4) {
            if (sEntityCount < MAX_ENTITIES) {
                sEntities[sEntityCount++] = (EntityEdit){ { (u8)a, (u8)b, (u8)c, TRUE, 0, 0, 0, 0 },
                                                          (s8)Clamp(value, -DELTA_LIMIT, DELTA_LIMIT) };
            }
        } else if (sscanf(line, "ent %x %x %x %x %x %u %u %d", &a, &b, &c, &d, &e, &f, &g, &value) == 8) {
            if (sEntityCount < MAX_ENTITIES && f < 256 && g < 256) {
                sEntities[sEntityCount++] =
                    (EntityEdit){ { (u8)a, (u8)b, (u8)c, FALSE, (u8)d, (u8)e, (u8)f, (u8)g },
                                  (s8)Clamp(value, -DELTA_LIMIT, DELTA_LIMIT) };
            }
        }
    }
    fclose(file);
    ++sRevision;
}

bool32 PortStereoEdits_Save(void) {
    if (!sDirty) {
        return TRUE;
    }
    FILE* file = fopen(STEREO_EDITS_FILE ".tmp", "w");
    if (file == NULL) {
        return FALSE;
    }
    fputs("# The Minish Cap 3DS: stereo 3D relief corrections (developer tools, 3D editor)\n"
          "# cell <area> <room> <b|t layer> <d add|s set> <row> <col0> <col1> <value>\n"
          "# ent <kind> <id> <type> all <delta> | ent <kind> <id> <type> <area> <room> <col> <row> <delta>\n",
          file);
    for (int i = 0; i < sRoomCount; ++i) {
        const RoomEdits* edits = sRooms[i];
        for (int layer = 0; layer < 2; ++layer) {
            for (int row = 0; row < SIDE; ++row) {
                const u8* flags = &edits->flags[layer][row * SIDE];
                const s8* value = &edits->value[layer][row * SIDE];
                for (int col = 0; col < SIDE;) {
                    if (!(flags[col] & CELL_HAS)) {
                        ++col;
                        continue;
                    }
                    int end = col;
                    while (end + 1 < SIDE && flags[end + 1] == flags[col] && value[end + 1] == value[col]) {
                        ++end;
                    }
                    fprintf(file, "cell %02x %02x %c %c %d %d %d %d\n", edits->area, edits->room, layer ? 't' : 'b',
                            (flags[col] & CELL_SET) ? 's' : 'd', row, col, end, value[col]);
                    col = end + 1;
                }
            }
        }
    }
    for (int i = 0; i < sEntityCount; ++i) {
        const PortStereoEntityKey* k = &sEntities[i].key;
        if (k->all) {
            fprintf(file, "ent %02x %02x %02x all %d\n", k->kind, k->id, k->type, sEntities[i].delta);
        } else {
            fprintf(file, "ent %02x %02x %02x %02x %02x %u %u %d\n", k->kind, k->id, k->type, k->area, k->room,
                    k->col, k->row, sEntities[i].delta);
        }
    }
    if (fclose(file) != 0) {
        return FALSE;
    }
    remove(STEREO_EDITS_FILE);
    if (rename(STEREO_EDITS_FILE ".tmp", STEREO_EDITS_FILE) != 0) {
        return FALSE;
    }
    sDirty = FALSE;
    return TRUE;
}

u32 PortStereoEdits_Revision(void) {
    return sRevision;
}

void PortStereoEdits_ApplyRoom(int area, int room, int cols, int rows, s8* height, s8* heightTop, s8* ground,
                               s8 unknownGround) {
    PortStereoEdits_Load();
    const RoomEdits* edits = FindRoom(area, room, FALSE);
    if (edits == NULL) {
        return;
    }
    for (int row = 0; row < rows && row < SIDE; ++row) {
        for (int col = 0; col < cols && col < SIDE; ++col) {
            const int at = row * cols + col, cell = row * SIDE + col;
            const u8 bottom = edits->flags[0][cell], top = edits->flags[1][cell];
            if (bottom & CELL_SET) {
                height[at] = ground[at] = edits->value[0][cell];
            } else if (bottom & CELL_HAS) {
                height[at] = (s8)Clamp(height[at] + edits->value[0][cell], -100, 100);
                if (ground[at] != unknownGround) {
                    ground[at] = (s8)Clamp(ground[at] + edits->value[0][cell], -100, 100);
                }
            }
            if (top & CELL_SET) {
                heightTop[at] = edits->value[1][cell];
            } else if (top & CELL_HAS) {
                heightTop[at] = (s8)Clamp(heightTop[at] + edits->value[1][cell], -100, 100);
            }
        }
    }
}

int PortStereoEdits_CellEdited(int area, int room, int col, int row, int layers) {
    static const RoomEdits* cached;
    static int cachedArea = -1, cachedRoom = -1;
    static u32 cachedRevision;
    if (cachedArea != area || cachedRoom != room || cachedRevision != sRevision) {
        cached = FindRoom(area, room, FALSE);
        cachedArea = area;
        cachedRoom = room;
        cachedRevision = sRevision;
    }
    if (cached == NULL || col < 0 || row < 0 || col >= SIDE || row >= SIDE) {
        return 0;
    }
    int edited = 0;
    for (int layer = 0; layer < 2; ++layer) {
        if ((layers & (1 << layer)) && (cached->flags[layer][row * SIDE + col] & CELL_HAS)) {
            edited |= 1 << layer;
        }
    }
    return edited;
}

void PortStereoEdits_Step(int area, int room, int layers, int col0, int row0, int col1, int row1, int step) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, TRUE);
    if (edits != NULL && step != 0 && ClipRect(&col0, &row0, &col1, &row1)) {
        StepCells(edits, layers, col0, row0, col1, row1, step);
        Changed();
    }
}

void PortStereoEdits_Reset(int area, int room, int layers, int col0, int row0, int col1, int row1) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, FALSE);
    if (edits == NULL || !ClipRect(&col0, &row0, &col1, &row1)) {
        return;
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (layers & (1 << layer)) {
            for (int row = row0; row <= row1; ++row) {
                memset(&edits->flags[layer][row * SIDE + col0], 0, (size_t)(col1 - col0 + 1));
                memset(&edits->value[layer][row * SIDE + col0], 0, (size_t)(col1 - col0 + 1));
            }
        }
    }
    Changed();
}

void PortStereoEdits_Set(int area, int room, int layers, int col0, int row0, int col1, int row1, int height) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, TRUE);
    if (edits == NULL || !ClipRect(&col0, &row0, &col1, &row1)) {
        return;
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (layers & (1 << layer)) {
            for (int row = row0; row <= row1; ++row) {
                SetCells(edits, layer, row, col0, col1, TRUE, Clamp(height, -HEIGHT_LIMIT, HEIGHT_LIMIT));
            }
        }
    }
    Changed();
}

static bool32 SameKey(const PortStereoEntityKey* a, const PortStereoEntityKey* b) {
    if (a->kind != b->kind || a->id != b->id || a->type != b->type || a->all != b->all) {
        return FALSE;
    }
    return a->all || (a->area == b->area && a->room == b->room && a->col == b->col && a->row == b->row);
}

int PortStereoEdits_EntityDelta(int area, int room, u8 kind, u8 id, u8 type, int col, int row) {
    int all = 0;
    for (int i = 0; i < sEntityCount; ++i) {
        const PortStereoEntityKey* k = &sEntities[i].key;
        if (k->kind != kind || k->id != id || k->type != type) {
            continue;
        }
        if (k->all) {
            all = sEntities[i].delta;
        } else if (k->area == area && k->room == room && abs(k->col - col) <= ENTITY_REACH &&
                   abs(k->row - row) <= ENTITY_REACH) {
            return sEntities[i].delta;
        }
    }
    return all;
}

int PortStereoEdits_KeyDelta(const PortStereoEntityKey* key) {
    for (int i = 0; i < sEntityCount; ++i) {
        if (SameKey(&sEntities[i].key, key)) {
            return sEntities[i].delta;
        }
    }
    return 0;
}

int PortStereoEdits_AdjustEntity(const PortStereoEntityKey* key, int step, bool32 clear) {
    PortStereoEdits_Load();
    int i = 0;
    while (i < sEntityCount && !SameKey(&sEntities[i].key, key)) {
        ++i;
    }
    if (i == sEntityCount) {
        if (clear || step == 0 || sEntityCount >= MAX_ENTITIES) {
            return 0;
        }
        ++sEntityCount;
        sEntities[i] = (EntityEdit){ *key, 0 };
        if (key->all) {
            PortStereoEntityKey* k = &sEntities[i].key;
            k->area = k->room = k->col = k->row = 0;
        }
    }
    const int delta = clear ? 0 : Clamp(sEntities[i].delta + step, -DELTA_LIMIT, DELTA_LIMIT);
    sEntities[i].delta = (s8)delta;
    if (delta == 0) {
        sEntities[i] = sEntities[--sEntityCount];
    }
    Changed();
    return delta;
}
