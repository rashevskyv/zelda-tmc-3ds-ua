/**
 * @file port_stereo_edits.c
 * @brief Stereoscopic 3D: hand-made corrections to the measured relief.
 *
 * See port_stereo_edits.h. The file is plain text, one edit per line, so it
 * can be read, diffed and folded into the code later:
 *
 *   rect <area> <room> <layers> <col0> <row0> <col1> <row1> <delta>
 *   ent <kind> <id> <type> all <delta>
 *   ent <kind> <id> <type> <area> <room> <col> <row> <delta>
 *
 * Area, room, kind, id and type are hex, as the developer overlay shows them;
 * the rest decimal. Layers: 1 bottom, 2 top, 3 both.
 */
#include "port_stereo_edits.h"

#include <stdio.h>
#include <stdlib.h>

#define STEREO_EDITS_FILE "stereo_edits.txt"

enum { MAX_RECTS = 512, MAX_ENTITIES = 256, ENTITY_REACH = 2, DELTA_LIMIT = 12 };

typedef struct {
    u8 area, room, layers;
    u8 col0, row0, col1, row1;
    s8 delta;
} RectEdit;

typedef struct {
    PortStereoEntityKey key;
    s8 delta;
} EntityEdit;

static RectEdit sRects[MAX_RECTS];
static int sRectCount;
static EntityEdit sEntities[MAX_ENTITIES];
static int sEntityCount;
static bool32 sLoaded;
static bool32 sDirty;
static u32 sRevision = 1;

static int Clamp(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
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
        int delta;
        if (sscanf(line, "rect %x %x %u %u %u %u %u %d", &a, &b, &c, &d, &e, &f, &g, &delta) == 8) {
            if (sRectCount < MAX_RECTS && c >= 1 && c <= 3 && d <= f && e <= g && f < 256 && g < 256) {
                sRects[sRectCount++] = (RectEdit){ (u8)a, (u8)b, (u8)c, (u8)d, (u8)e, (u8)f, (u8)g,
                                                   (s8)Clamp(delta, -DELTA_LIMIT, DELTA_LIMIT) };
            }
        } else if (sscanf(line, "ent %x %x %x all %d", &a, &b, &c, &delta) == 4) {
            if (sEntityCount < MAX_ENTITIES) {
                sEntities[sEntityCount++] = (EntityEdit){ { (u8)a, (u8)b, (u8)c, TRUE, 0, 0, 0, 0 },
                                                          (s8)Clamp(delta, -DELTA_LIMIT, DELTA_LIMIT) };
            }
        } else if (sscanf(line, "ent %x %x %x %x %x %u %u %d", &a, &b, &c, &d, &e, &f, &g, &delta) == 8) {
            if (sEntityCount < MAX_ENTITIES && f < 256 && g < 256) {
                sEntities[sEntityCount++] =
                    (EntityEdit){ { (u8)a, (u8)b, (u8)c, FALSE, (u8)d, (u8)e, (u8)f, (u8)g },
                                  (s8)Clamp(delta, -DELTA_LIMIT, DELTA_LIMIT) };
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
          "# rect <area> <room> <layers 1 bottom 2 top 3 both> <col0> <row0> <col1> <row1> <delta>\n"
          "# ent <kind> <id> <type> all <delta> | ent <kind> <id> <type> <area> <room> <col> <row> <delta>\n",
          file);
    for (int i = 0; i < sRectCount; ++i) {
        const RectEdit* r = &sRects[i];
        fprintf(file, "rect %02x %02x %u %u %u %u %u %d\n", r->area, r->room, r->layers, r->col0, r->row0, r->col1,
                r->row1, r->delta);
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
    const bool32 ok = fclose(file) == 0;
    if (!ok) {
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
    for (int i = 0; i < sRectCount; ++i) {
        const RectEdit* r = &sRects[i];
        if (r->area != area || r->room != room) {
            continue;
        }
        for (int row = r->row0; row <= r->row1 && row < rows; ++row) {
            for (int col = r->col0; col <= r->col1 && col < cols; ++col) {
                const int at = row * cols + col;
                if (r->layers & PORT_STEREO_EDIT_BOTTOM) {
                    height[at] = (s8)Clamp(height[at] + r->delta, -100, 100);
                    if (ground[at] != unknownGround) {
                        ground[at] = (s8)Clamp(ground[at] + r->delta, -100, 100);
                    }
                }
                if (r->layers & PORT_STEREO_EDIT_TOP) {
                    heightTop[at] = (s8)Clamp(heightTop[at] + r->delta, -100, 100);
                }
            }
        }
    }
}

int PortStereoEdits_CellDelta(int area, int room, int col, int row, int layers) {
    int sum = 0;
    for (int i = 0; i < sRectCount; ++i) {
        const RectEdit* r = &sRects[i];
        if (r->area == area && r->room == room && (r->layers & layers) && col >= r->col0 && col <= r->col1 &&
            row >= r->row0 && row <= r->row1) {
            sum += r->delta;
        }
    }
    return sum;
}

static int FindRect(int area, int room, int layers, int col0, int row0, int col1, int row1) {
    for (int i = 0; i < sRectCount; ++i) {
        const RectEdit* r = &sRects[i];
        if (r->area == area && r->room == room && r->layers == layers && r->col0 == col0 && r->row0 == row0 &&
            r->col1 == col1 && r->row1 == row1) {
            return i;
        }
    }
    return -1;
}

int PortStereoEdits_RectDelta(int area, int room, int layers, int col0, int row0, int col1, int row1) {
    const int i = FindRect(area, room, layers, col0, row0, col1, row1);
    return i >= 0 ? sRects[i].delta : 0;
}

int PortStereoEdits_AdjustRect(int area, int room, int layers, int col0, int row0, int col1, int row1, int step,
                               bool32 clear) {
    PortStereoEdits_Load();
    if (col0 < 0 || row0 < 0 || col1 > 255 || row1 > 255 || col0 > col1 || row0 > row1) {
        return 0;
    }
    int i = FindRect(area, room, layers, col0, row0, col1, row1);
    if (i < 0) {
        if (clear || step == 0 || sRectCount >= MAX_RECTS) {
            return 0;
        }
        i = sRectCount++;
        sRects[i] = (RectEdit){ (u8)area, (u8)room, (u8)layers, (u8)col0, (u8)row0, (u8)col1, (u8)row1, 0 };
    }
    const int delta = clear ? 0 : Clamp(sRects[i].delta + step, -DELTA_LIMIT, DELTA_LIMIT);
    sRects[i].delta = (s8)delta;
    if (delta == 0) {
        sRects[i] = sRects[--sRectCount];
    }
    sDirty = TRUE;
    ++sRevision;
    return delta;
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
    sDirty = TRUE;
    ++sRevision;
    return delta;
}
