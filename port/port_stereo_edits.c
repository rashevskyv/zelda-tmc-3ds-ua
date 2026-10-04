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
 *   tile <b|t> <hash> <quarter> <d|s> <value>
 *   screen <key> <bg> <depth>
 *   ent <kind> <id> <type> all <delta>
 *   ent <kind> <id> <type> <area> <room> <col> <row> <delta>
 *
 * b/t is the bottom or top map layer, d adds the value to the measured height
 * and s replaces it. A tile rule holds for every cell, in any room, whose
 * 16x16 map tile looks the same -- `hash` is PortStereoEdits_TileHash of its
 * graphics, `quarter` which 8x8 cell of it (0 top-left, 1 top-right, 2, 3) --
 * and room cells are applied over the rules. The key leaves the pixels out:
 * animated tiles (water, flowers) change them every few frames. Area, room, kind, id and type are hex, as the developer
 * overlay shows them; the rest decimal. The first editor build wrote
 * rectangles instead (`rect <area> <room> <layers> <col0> <row0> <col1> <row1>
 * <delta>`); those are still read, as additions.
 */
#include "port_stereo_edits.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEREO_EDITS_FILE "stereo_edits.txt"

enum {
    SIDE = PORT_STEREO_EDIT_SIDE,
    MAX_ROOMS = 96,
    MAX_RULES = 8192,
    RULE_SLOTS = 16384, /* power of two, at most half full */
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

typedef struct {
    u32 hash;
    u8 layer, quarter, flags;
    s8 value;
} TileRule;

static RoomEdits* sRooms[MAX_ROOMS];
static int sRoomCount;
static EntityEdit sEntities[MAX_ENTITIES];
static int sEntityCount;
static TileRule sRules[MAX_RULES];
enum { MAX_SCREENS = 128 };
static struct {
    u32 key;
    u8 bg;
    s8 depth;
} sScreens[MAX_SCREENS];
static int sScreenCount;
static int sRuleCount;
/* Rule index + 1 by key, rebuilt when the rules change. */
static u16 sRuleSlots[RULE_SLOTS];
static bool32 sRuleSlotsStale = TRUE;
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

static u32 RuleKey(u32 hash, int layer, int quarter) {
    return (hash ^ ((u32)layer * 0x9e3779b9u) ^ ((u32)quarter * 0x85ebca6bu)) * 0xc2b2ae35u;
}

static void RebuildRuleSlots(void) {
    memset(sRuleSlots, 0, sizeof(sRuleSlots));
    for (int i = 0; i < sRuleCount; ++i) {
        u32 slot = RuleKey(sRules[i].hash, sRules[i].layer, sRules[i].quarter) & (RULE_SLOTS - 1);
        while (sRuleSlots[slot] != 0) {
            slot = (slot + 1) & (RULE_SLOTS - 1);
        }
        sRuleSlots[slot] = (u16)(i + 1);
    }
    sRuleSlotsStale = FALSE;
}

static TileRule* FindRule(u32 hash, int layer, int quarter) {
    if (sRuleSlotsStale) {
        RebuildRuleSlots();
    }
    u32 slot = RuleKey(hash, layer, quarter) & (RULE_SLOTS - 1);
    while (sRuleSlots[slot] != 0) {
        TileRule* rule = &sRules[sRuleSlots[slot] - 1];
        if (rule->hash == hash && rule->layer == layer && rule->quarter == quarter) {
            return rule;
        }
        slot = (slot + 1) & (RULE_SLOTS - 1);
    }
    return NULL;
}

static void PutRule(u32 hash, int layer, int quarter, bool32 set, int value) {
    TileRule* rule = FindRule(hash, layer, quarter);
    if (rule == NULL) {
        if (sRuleCount >= MAX_RULES) {
            return;
        }
        rule = &sRules[sRuleCount++];
        sRuleSlotsStale = TRUE;
    }
    *rule = (TileRule){ hash, (u8)layer, (u8)quarter, (u8)(set ? CELL_SET : 0),
                        (s8)Clamp(value, -HEIGHT_LIMIT, HEIGHT_LIMIT) };
}

/* One line of the edits file. `onlyRoom` >= 0 keeps cell lines of that
 * area << 8 | room only; `kinds` is a mask of which lines to take. */
static void ParseLine(const char* line, int kinds, int onlyRoom) {
    unsigned a, b, c, d, e, f, g;
    char layer, mode;
    int value;
    if ((kinds & PORT_STEREO_EXPORT_CELLS) &&
        sscanf(line, "cell %x %x %c %c %u %u %u %d", &a, &b, &layer, &mode, &c, &d, &e, &value) == 8) {
        int col0 = (int)d, row0 = (int)c, col1 = (int)e, row1 = (int)c;
        if (onlyRoom >= 0 && (int)((a << 8) | b) != onlyRoom) {
            return;
        }
        RoomEdits* edits = FindRoom((int)a, (int)b, TRUE);
        if (edits != NULL && (layer == 'b' || layer == 't') && (mode == 'd' || mode == 's') &&
            ClipRect(&col0, &row0, &col1, &row1)) {
            SetCells(edits, layer == 't', row0, col0, col1, mode == 's', Clamp(value, -HEIGHT_LIMIT, HEIGHT_LIMIT));
        }
    } else if ((kinds & PORT_STEREO_EXPORT_TILES) &&
               sscanf(line, "tile %c %x %u %c %d", &layer, &a, &b, &mode, &value) == 5) {
        if ((layer == 'b' || layer == 't') && (mode == 'd' || mode == 's') && b < 4) {
            PutRule(a, layer == 't', (int)b, mode == 's', value);
        }
    } else if ((kinds & PORT_STEREO_EXPORT_CELLS) && onlyRoom < 0 &&
               sscanf(line, "rect %x %x %u %u %u %u %u %d", &a, &b, &c, &d, &e, &f, &g, &value) == 8) {
        int col0 = (int)d, row0 = (int)e, col1 = (int)f, row1 = (int)g;
        RoomEdits* edits = FindRoom((int)a, (int)b, TRUE);
        if (edits != NULL && ClipRect(&col0, &row0, &col1, &row1)) {
            StepCells(edits, (int)c & PORT_STEREO_EDIT_BOTH, col0, row0, col1, row1, value);
        }
    } else if ((kinds & PORT_STEREO_EXPORT_SCREENS) && sscanf(line, "screen %x %u %d", &a, &b, &value) == 3) {
        if (b < 4 && sScreenCount < MAX_SCREENS) {
            sScreens[sScreenCount].key = a;
            sScreens[sScreenCount].bg = (u8)b;
            sScreens[sScreenCount].depth = (s8)Clamp(value, -4, 15);
            ++sScreenCount;
        }
    } else if ((kinds & PORT_STEREO_EXPORT_ENTITIES) && sscanf(line, "ent %x %x %x all %d", &a, &b, &c, &value) == 4) {
        if (sEntityCount < MAX_ENTITIES) {
            sEntities[sEntityCount++] = (EntityEdit){ { (u8)a, (u8)b, (u8)c, TRUE, 0, 0, 0, 0 },
                                                      (s8)Clamp(value, -DELTA_LIMIT, DELTA_LIMIT) };
        }
    } else if ((kinds & PORT_STEREO_EXPORT_ENTITIES) &&
               sscanf(line, "ent %x %x %x %x %x %u %u %d", &a, &b, &c, &d, &e, &f, &g, &value) == 8) {
        if (sEntityCount < MAX_ENTITIES && f < 256 && g < 256) {
            sEntities[sEntityCount++] = (EntityEdit){ { (u8)a, (u8)b, (u8)c, FALSE, (u8)d, (u8)e, (u8)f, (u8)g },
                                                      (s8)Clamp(value, -DELTA_LIMIT, DELTA_LIMIT) };
        }
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
        ParseLine(line, PORT_STEREO_EXPORT_ALL, -1);
    }
    fclose(file);
    ++sRevision;
}

/* Where edits are written: a file, or a growing buffer. */
typedef struct {
    FILE* file;
    char* text;
    size_t length, capacity;
    bool32 failed;
} Out;

static void OutPrintf(Out* out, const char* format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    if (out->file != NULL) {
        fputs(line, out->file);
        return;
    }
    if (out->length + (size_t)n + 1 > out->capacity) {
        const size_t capacity = (out->capacity + (size_t)n + 1) * 2;
        char* text = realloc(out->text, capacity);
        if (text == NULL) {
            out->failed = TRUE;
            return;
        }
        out->text = text;
        out->capacity = capacity;
    }
    memcpy(out->text + out->length, line, (size_t)n + 1);
    out->length += (size_t)n;
}

static void WriteRoomCells(Out* out, const RoomEdits* edits) {
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
                OutPrintf(out, "cell %02x %02x %c %c %d %d %d %d\n", edits->area, edits->room, layer ? 't' : 'b',
                          (flags[col] & CELL_SET) ? 's' : 'd', row, col, end, value[col]);
                col = end + 1;
            }
        }
    }
}

static void Write(Out* out, int kinds, int onlyRoom) {
    if (kinds & PORT_STEREO_EXPORT_CELLS) {
        for (int i = 0; i < sRoomCount; ++i) {
            if (onlyRoom < 0 || ((sRooms[i]->area << 8) | sRooms[i]->room) == onlyRoom) {
                WriteRoomCells(out, sRooms[i]);
            }
        }
    }
    if (kinds & PORT_STEREO_EXPORT_TILES) {
        for (int i = 0; i < sRuleCount; ++i) {
            const TileRule* r = &sRules[i];
            OutPrintf(out, "tile %c %08lx %u %c %d\n", r->layer ? 't' : 'b', (unsigned long)r->hash, r->quarter,
                      (r->flags & CELL_SET) ? 's' : 'd', r->value);
        }
    }
    if (kinds & PORT_STEREO_EXPORT_SCREENS) {
        for (int i = 0; i < sScreenCount; ++i) {
            OutPrintf(out, "screen %08lx %u %d\n", (unsigned long)sScreens[i].key, sScreens[i].bg, sScreens[i].depth);
        }
    }
    if (kinds & PORT_STEREO_EXPORT_ENTITIES) {
        for (int i = 0; i < sEntityCount; ++i) {
            const PortStereoEntityKey* k = &sEntities[i].key;
            if (k->all) {
                OutPrintf(out, "ent %02x %02x %02x all %d\n", k->kind, k->id, k->type, sEntities[i].delta);
            } else {
                OutPrintf(out, "ent %02x %02x %02x %02x %02x %u %u %d\n", k->kind, k->id, k->type, k->area, k->room,
                          k->col, k->row, sEntities[i].delta);
            }
        }
    }
}

bool32 PortStereoEdits_Save(void) {
    if (!sDirty) {
        return TRUE;
    }
    Out out = { fopen(STEREO_EDITS_FILE ".tmp", "w"), NULL, 0, 0, FALSE };
    if (out.file == NULL) {
        return FALSE;
    }
    fputs("# The Minish Cap 3DS: stereo 3D relief corrections (3D editor, PC editor)\n"
          "# cell <area> <room> <b|t layer> <d add|s set> <row> <col0> <col1> <value>\n"
          "# tile <b|t> <graphics hash> <quarter> <d|s> <value>\n"
          "# screen <key> <bg 0-3> <depth>   (menus and other screens that are not a room)\n"
          "# ent <kind> <id> <type> all <delta> | ent <kind> <id> <type> <area> <room> <col> <row> <delta>\n",
          out.file);
    Write(&out, PORT_STEREO_EXPORT_ALL, -1);
    if (fclose(out.file) != 0) {
        return FALSE;
    }
    remove(STEREO_EDITS_FILE);
    if (rename(STEREO_EDITS_FILE ".tmp", STEREO_EDITS_FILE) != 0) {
        return FALSE;
    }
    sDirty = FALSE;
    return TRUE;
}

char* PortStereoEdits_Export(int kinds, int area, int room, size_t* length) {
    PortStereoEdits_Load();
    Out out = { NULL, NULL, 0, 0, FALSE };
    OutPrintf(&out, "# rev %lu\n", (unsigned long)sRevision);
    Write(&out, kinds, (kinds & PORT_STEREO_EXPORT_CELLS) && area >= 0 ? (area << 8) | room : -1);
    if (out.failed) {
        free(out.text);
        return NULL;
    }
    *length = out.length;
    return out.text;
}

void PortStereoEdits_Import(int kinds, int area, int room, const char* text, size_t length) {
    PortStereoEdits_Load();
    if (kinds & PORT_STEREO_EXPORT_CELLS) {
        RoomEdits* edits = FindRoom(area, room, FALSE);
        if (edits != NULL) {
            memset(edits->flags, 0, sizeof(edits->flags));
            memset(edits->value, 0, sizeof(edits->value));
        }
    }
    if (kinds & PORT_STEREO_EXPORT_TILES) {
        sRuleCount = 0;
        sRuleSlotsStale = TRUE;
    }
    if (kinds & PORT_STEREO_EXPORT_ENTITIES) {
        sEntityCount = 0;
    }
    if (kinds & PORT_STEREO_EXPORT_SCREENS) {
        sScreenCount = 0;
    }
    const char* end = text + length;
    while (text < end) {
        const char* newline = memchr(text, '\n', (size_t)(end - text));
        const size_t n = (size_t)((newline != NULL ? newline : end) - text);
        char line[160];
        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = '\0';
            ParseLine(line, kinds, (kinds & PORT_STEREO_EXPORT_CELLS) ? (area << 8) | room : -1);
        }
        text += n + 1;
    }
    Changed();
}

u32 PortStereoEdits_TileHash(const u16* subTiles, u32 tilesetKey) {
    u32 hash = 2166136261u;
    for (int i = 0; i < 4; ++i) {
        hash = (hash ^ ((tilesetKey >> (8 * i)) & 0xff)) * 16777619u;
    }
    for (int q = 0; q < 4; ++q) {
        hash = (hash ^ (subTiles[q] & 0xff)) * 16777619u;
        hash = (hash ^ (subTiles[q] >> 8)) * 16777619u;
    }
    return hash != 0 ? hash : 1;
}

u32 PortStereoEdits_Revision(void) {
    return sRevision;
}

/* One layer's edit of one cell: replace or add. */
static void ApplyOne(u8 flags, s8 value, s8* height, s8* ground, s8 unknownGround) {
    if (flags & CELL_SET) {
        *height = value;
        if (ground != NULL) {
            *ground = value;
        }
    } else {
        *height = (s8)Clamp(*height + value, -100, 100);
        if (ground != NULL && *ground != unknownGround) {
            *ground = (s8)Clamp(*ground + value, -100, 100);
        }
    }
}

void PortStereoEdits_ApplyRoom(int area, int room, int cols, int rows, s8* height, s8* heightTop, s8* ground,
                               s8 unknownGround, const u32* tileHashBottom, const u32* tileHashTop) {
    PortStereoEdits_Load();
    if (sRuleCount > 0) {
        for (int row = 0; row < rows && row < SIDE; ++row) {
            for (int col = 0; col < cols && col < SIDE; ++col) {
                const int at = row * cols + col, tile = (col >> 1) | ((row >> 1) << 6);
                const int quarter = (col & 1) | ((row & 1) << 1);
                const TileRule* rule;
                if (tileHashBottom != NULL && tileHashBottom[tile] != 0 &&
                    (rule = FindRule(tileHashBottom[tile], 0, quarter)) != NULL) {
                    ApplyOne(rule->flags, rule->value, &height[at], &ground[at], unknownGround);
                }
                if (tileHashTop != NULL && tileHashTop[tile] != 0 &&
                    (rule = FindRule(tileHashTop[tile], 1, quarter)) != NULL) {
                    ApplyOne(rule->flags, rule->value, &heightTop[at], NULL, unknownGround);
                }
            }
        }
    }
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

void PortStereoEdits_Step(int area, int room, int layers, const u8* selection, int step) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, TRUE);
    if (edits == NULL || step == 0) {
        return;
    }
    for (int row = 0; row < SIDE; ++row) {
        for (int col = 0; col < SIDE; ++col) {
            if (selection[row * SIDE + col]) {
                StepCells(edits, layers, col, row, col, row, step);
            }
        }
    }
    Changed();
}

void PortStereoEdits_Reset(int area, int room, int layers, const u8* selection) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, FALSE);
    if (edits == NULL) {
        return;
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (!(layers & (1 << layer))) {
            continue;
        }
        for (int cell = 0; cell < SIDE * SIDE; ++cell) {
            if (selection[cell]) {
                edits->flags[layer][cell] = 0;
                edits->value[layer][cell] = 0;
            }
        }
    }
    Changed();
}

void PortStereoEdits_Set(int area, int room, int layers, const u8* selection, int height) {
    PortStereoEdits_Load();
    RoomEdits* edits = FindRoom(area, room, TRUE);
    if (edits == NULL) {
        return;
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (!(layers & (1 << layer))) {
            continue;
        }
        for (int row = 0; row < SIDE; ++row) {
            for (int col = 0; col < SIDE; ++col) {
                if (selection[row * SIDE + col]) {
                    SetCells(edits, layer, row, col, col, TRUE, Clamp(height, -HEIGHT_LIMIT, HEIGHT_LIMIT));
                }
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

bool32 PortStereoEdits_ScreenDepth(u32 key, int bg, int* depth) {
    PortStereoEdits_Load();
    for (int i = 0; i < sScreenCount; ++i) {
        if (sScreens[i].key == key && sScreens[i].bg == bg) {
            *depth = sScreens[i].depth;
            return TRUE;
        }
    }
    return FALSE;
}

void PortStereoEdits_SetScreenDepth(u32 key, int bg, bool32 set, int depth) {
    PortStereoEdits_Load();
    if (bg < 0 || bg >= 4) {
        return;
    }
    for (int i = 0; i < sScreenCount; ++i) {
        if (sScreens[i].key == key && sScreens[i].bg == bg) {
            if (set) {
                sScreens[i].depth = (s8)Clamp(depth, -4, 15);
            } else {
                sScreens[i] = sScreens[--sScreenCount];
            }
            Changed();
            return;
        }
    }
    if (set && sScreenCount < MAX_SCREENS) {
        sScreens[sScreenCount].key = key;
        sScreens[sScreenCount].bg = (u8)bg;
        sScreens[sScreenCount].depth = (s8)Clamp(depth, -4, 15);
        ++sScreenCount;
        Changed();
    }
}
