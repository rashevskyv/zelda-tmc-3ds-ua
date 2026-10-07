/**
 * @file port_stereo_editor.c
 * @brief Stereoscopic 3D editor: correct the relief by hand on the touch screen.
 *
 * See port_stereo_editor.h. Everything here runs on the main thread between
 * frames, where the room and the entity lists are the engine's own; only the
 * status line is read from the bottom-screen painter's thread, through a
 * double buffer.
 */
#include "port_stereo_editor.h"
#include "port_stereo.h"
#include "port_stereo_edits.h"
#include "port_stereo_link.h"

#include "global.h"
#include "entity.h"
#include "main.h"
#include "map.h"
#include "room.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's frame as the renderer last drew it: 240x160, or wider (266 in
 * WIDE) -- the view, the cells and the stylus all count from its left edge,
 * which is where gRoomControls.scroll_x is. */
static int sFrameW = 240, sFrameH = 160;
enum { MAX_ZOOM = 4, DRAG_SLOP = 3, LINE_SIZE = 160, DOUBLE_TAP_FRAMES = 30 };
enum { SEL_NONE, SEL_CELLS, SEL_ENTITY };
enum { SIDE = PORT_STEREO_EDIT_SIDE };

static volatile bool sOpen;
static volatile bool sHelp;
static int sZoom = 1;
static float sPanX, sPanY;
static int sLayers = PORT_STEREO_EDIT_BOTH;
static int sSel;
/* Selected cells of room sArea:sRoom, how many, and what they span. */
static u8 sSelection[SIDE * SIDE];
static int sSelected;
static int sArea, sRoom;
static int sCol0, sRow0, sCol1, sRow1;
static PortStereoEntityKey sKey;
static bool sTouching, sMoved;
/* The tap that opened the editor is still on the screen at first. */
static bool sWaitRelease;
static int sTouchX0, sTouchY0;
static unsigned sRepeat;
/* Colour the cells by how high they stand (SELECT turns it off). */
static bool sHeatmap = true;
/* A second tap on the same sprite or cell soon after the first picks all of
 * its kind: every entity like it, every cell of the same map tile. */
static unsigned sFrame, sTapFrame;
static const Entity* sTapEntity;
static int sTapCol = -1, sTapRow = -1;
static bool sSaveFailed;
static unsigned sSelectionRevision;

/* The room list: closed, the areas, or one area's rooms. */
enum { LIST_CLOSED, LIST_AREAS, LIST_ROOMS };
enum { MAX_LIST = 160 };
static int sList;
static int sListItems[MAX_LIST];
static int sListCount, sListCursor, sListTop, sListArea;
static char sListLines[2][PORT_STEREO_EDITOR_LIST_ROWS][48];
static char sListTitle[2][48];
static volatile int sListShown[2], sListCursorShown[2], sListTopShown[2], sListCountShown[2];
static volatile int sListBuffer;

/* Names as the decompilation calls the areas. */
static const char* const kAreaNames[] = {
    "Minish Woods", "Minish Village", "Hyrule Town", "Hyrule Field", "Castor Wilds", "Ruins", "Mt. Crenel",
    "Castle Garden", "Cloud Tops", "Royal Valley", "Veil Falls", "Lake Hylia", "Lake Woods Cave", "Beanstalks",
    "Empty", "Hyrule Dig Caves", "Melari's Mine", "Minish Paths", "Crenel Minish Paths", "Dig Caves",
    "Crenel Dig Cave", "Festival Town", "Veil Falls Dig Cave", "Castor Wilds Dig Cave", "Outer Fortress of Winds",
    "Hylia Dig Caves", "Veil Falls Top", NULL, NULL, NULL, NULL, NULL, "Minish House Interiors",
    "House Interiors 1", "House Interiors 2", "House Interiors 3", "Tree Interiors", "Dojos", "Crenel Caves",
    "Minish Cracks", "House Interiors 4", "Great Fairies", "Castor Caves", "Castor Darknut", "Armos Interiors",
    "Town Minish Holes", "Minish Rafters", "Goron Cave", "Wind Tribe Tower", "Wind Tribe Tower Roof", "Caves",
    "Veil Falls Caves", "Royal Valley Graves", "Minish Caves", "Castle Garden Minish Holes", NULL, "Ezlo Cutscene",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "Hyrule Town Underground", "Garden Fountains",
    "Hyrule Castle Cellar", "Simon's Simulation", NULL, NULL, NULL, "Deepwood Shrine", "Deepwood Shrine Boss",
    "Deepwood Shrine Entry", NULL, NULL, NULL, NULL, NULL, "Cave of Flames", "Cave of Flames Boss", NULL, NULL,
    NULL, NULL, NULL, NULL, "Fortress of Winds", "Fortress of Winds Top", "Inner Mazaal", NULL, NULL, NULL, NULL,
    NULL, "Temple of Droplets", NULL, "Hyrule Town Minish Caves", NULL, NULL, NULL, NULL, NULL, "Royal Crypt",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "Palace of Winds", "Palace of Winds Boss", NULL, NULL, NULL, NULL,
    NULL, NULL, "Sanctuary", NULL, NULL, NULL, NULL, NULL, NULL, NULL, "Hyrule Castle", "Sanctuary Entrance",
    NULL, NULL, NULL, NULL, NULL, NULL, "Dark Hyrule Castle", "Dark Hyrule Castle Outside", "Vaati's Arms",
    "Vaati 3", "Vaati 2", "Dark Hyrule Castle Bridge",
};
enum { AREA_NAME_COUNT = sizeof(kAreaNames) / sizeof(kAreaNames[0]) };

static char sLines[2][LINE_SIZE];
static volatile int sLineBuffer;

static void OpenAreas(void);
static void PublishList(void);
static bool ListInput(uint32_t down, uint32_t held, bool touching, int touchX, int touchY);

/* Zoom 1 fits the whole frame into the 320x200 view. */
static float Scale(void) {
    const float fitW = (float)PORT_STEREO_EDITOR_VIEW_W / (float)sFrameW;
    const float fitH = (float)PORT_STEREO_EDITOR_VIEW_H / (float)sFrameH;
    return (fitW < fitH ? fitW : fitH) * (float)sZoom;
}

static float ClampF(float value, float lo, float hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static int Min(int a, int b) {
    return a < b ? a : b;
}

static int Max(int a, int b) {
    return a > b ? a : b;
}

static void ClampPan(void) {
    const float w = PORT_STEREO_EDITOR_VIEW_W / Scale(), h = PORT_STEREO_EDITOR_VIEW_H / Scale();
    const float fw = (float)sFrameW, fh = (float)sFrameH;
    sPanX = w >= fw ? (fw - w) / 2.0f : ClampF(sPanX, 0.0f, fw - w);
    sPanY = h >= fh ? (fh - h) / 2.0f : ClampF(sPanY, 0.0f, fh - h);
}

void PortStereoEditor_SetFrame(int width, int height) {
    if (width < 8 || height < 8 || (width == sFrameW && height == sFrameH)) {
        return;
    }
    sFrameW = width;
    sFrameH = height;
    ClampPan();
}

static bool InGame(void) {
    return gMain.task == TASK_GAME;
}

/* The room's top-left in screen pixels; cells are 8 pixels from there. */
static int RoomScreenX(void) {
    return (int)gRoomControls.origin_x - (int)gRoomControls.scroll_x;
}

static int RoomScreenY(void) {
    return (int)gRoomControls.origin_y - (int)gRoomControls.scroll_y;
}

static float ToGbaX(int touchX) {
    return sPanX + (float)touchX / Scale();
}

static float ToGbaY(int touchY) {
    return sPanY + (float)touchY / Scale();
}

static int CellCol(float gbaX) {
    return ((int)gbaX - RoomScreenX()) >> 3;
}

static int CellRow(float gbaY) {
    return ((int)gbaY - RoomScreenY()) >> 3;
}

static int RoomCols(void) {
    return Min(gRoomControls.width / 8, SIDE);
}

static int RoomRows(void) {
    return Min(gRoomControls.height / 8, SIDE);
}

static bool CellInRoom(int col, int row) {
    return col >= 0 && row >= 0 && col < RoomCols() && row < RoomRows();
}

static float EntityScreenX(const Entity* e) {
    return (float)((int)e->x.HALF.HI - (int)gRoomControls.scroll_x);
}

/* Where its feet are drawn: z is negative upwards. */
static float EntityScreenY(const Entity* e) {
    return (float)((int)e->y.HALF.HI - (int)gRoomControls.scroll_y + (int)e->z.HALF.HI);
}

static int EntityCol(const Entity* e) {
    return ((int)e->x.HALF.HI - (int)gRoomControls.origin_x) >> 3;
}

static int EntityRow(const Entity* e) {
    return ((int)e->y.HALF.HI - (int)gRoomControls.origin_y) >> 3;
}

/* Calls `visit` for every drawn entity; stops when it returns false. */
static void ForEachEntity(bool (*visit)(Entity* e, void* context), void* context) {
    for (int l = 0; l < 9; ++l) {
        LinkedList* list = &gEntityLists[l];
        if (list->first == NULL) {
            continue;
        }
        int guard = 0;
        for (Entity* e = list->first; e != NULL && e != (Entity*)list && guard < 512; e = e->next, ++guard) {
            if (e->kind == MANAGER || (e->flags & ENT_DELETED) || e->spriteSettings.draw == 0) {
                continue;
            }
            if (!visit(e, context)) {
                return;
            }
        }
    }
}

typedef struct {
    float x, y;
    float best;
    Entity* found;
} Pick;

/* A sprite is roughly a 16-pixel figure standing on its feet. */
static bool PickVisit(Entity* e, void* context) {
    Pick* pick = context;
    const float sx = EntityScreenX(e), sy = EntityScreenY(e);
    if (pick->x < sx - 10.0f || pick->x > sx + 10.0f || pick->y < sy - 22.0f || pick->y > sy + 4.0f) {
        return true;
    }
    const float dx = pick->x - sx, dy = pick->y - (sy - 8.0f);
    const float d = dx * dx + dy * dy;
    if (pick->found == NULL || d < pick->best) {
        pick->best = d;
        pick->found = e;
    }
    return true;
}

static bool KeyMatches(const PortStereoEntityKey* key, const Entity* e) {
    if (e->kind != key->kind || e->id != key->id || e->type != key->type) {
        return false;
    }
    return key->all || (key->area == gRoomControls.area && key->room == gRoomControls.room &&
                        abs(EntityCol(e) - key->col) <= 2 && abs(EntityRow(e) - key->row) <= 2);
}

static const char* LayerName(int layers) {
    return layers == PORT_STEREO_EDIT_BOTTOM ? "нижній" : layers == PORT_STEREO_EDIT_TOP ? "верхній" : "обидва";
}

/* The height a cell shows for the layers being edited. */
static bool CellHeight(int col, int row, int layers, int* height) {
    int bottom, top;
    if (!Port_Stereo_CellHeights(col, row, &bottom, &top)) {
        return false;
    }
    *height = layers == PORT_STEREO_EDIT_BOTTOM ? bottom : layers == PORT_STEREO_EDIT_TOP ? top
                                                                                          : (bottom > top ? bottom : top);
    return true;
}

/* The lowest and highest the selection stands, for the layers asked. */
static bool SelectionRange(int layers, int* lo, int* hi) {
    bool any = false;
    for (int row = sRow0; row <= sRow1; ++row) {
        for (int col = sCol0; col <= sCol1; ++col) {
            int h;
            if (sSelection[row * SIDE + col] && CellHeight(col, row, layers, &h)) {
                *lo = any && *lo < h ? *lo : h;
                *hi = any && *hi > h ? *hi : h;
                any = true;
            }
        }
    }
    return any;
}

static void PublishStatus(void) {
    const int next = !sLineBuffer;
    char* line = sLines[next];
    int lo = 0, hi = 0;
    if (sSaveFailed) {
        snprintf(line, LINE_SIZE, "Не вдалося зберегти stereo_edits.txt");
    } else if (!InGame()) {
        snprintf(line, LINE_SIZE, "Лише в грі");
    } else if (!Port_Stereo_ReliefLive()) {
        snprintf(line, LINE_SIZE, "%02X:%02X  повзунок 3D і РЕЛЬЄФ 3D", gRoomControls.area, gRoomControls.room);
    } else if (sSel == SEL_CELLS && SelectionRange(sLayers, &lo, &hi)) {
        char range[24];
        snprintf(range, sizeof(range), lo == hi ? "%d" : "%d..%d", lo, hi);
        if (sSelected == (sCol1 - sCol0 + 1) * (sRow1 - sRow0 + 1)) {
            snprintf(line, LINE_SIZE, "%02X:%02X  %d,%d-%d,%d  %s  %s", sArea, sRoom, sCol0, sRow0, sCol1, sRow1,
                     LayerName(sLayers), range);
        } else {
            snprintf(line, LINE_SIZE, "%02X:%02X  %d кл.  %s  %s", sArea, sRoom, sSelected, LayerName(sLayers), range);
        }
    } else if (sSel == SEL_ENTITY) {
        snprintf(line, LINE_SIZE, "%02X:%02X  об'єкт %02X %02X %02X  %s  %+d", gRoomControls.area, gRoomControls.room,
                 sKey.kind, sKey.id, sKey.type, sKey.all ? "усі" : "цей", PortStereoEdits_KeyDelta(&sKey));
    } else {
        snprintf(line, LINE_SIZE, "%02X:%02X", gRoomControls.area, gRoomControls.room);
    }
    sLineBuffer = next;
}

void PortStereoEditor_Open(void) {
    PortStereoEdits_Load();
    sSel = SEL_NONE;
    sHelp = false;
    sTouching = false;
    sWaitRelease = true;
    sRepeat = 0;
    sSaveFailed = false;
    ClampPan();
    PublishStatus();
    sList = LIST_CLOSED;
    PublishList();
    sOpen = true;
}

bool PortStereoEditor_IsOpen(void) {
    return sOpen;
}

bool PortStereoEditor_HelpOpen(void) {
    return sOpen && sHelp;
}

void PortStereoEditor_ShowHelp(void) {
    sHelp = true;
}

void PortStereoEditor_ShowRooms(void) {
    OpenAreas();
}

/* Zooms about the selection, or the middle of the view. */
static void ZoomTo(int zoom) {
    const float s = Scale();
    float cx = sPanX + PORT_STEREO_EDITOR_VIEW_W / s / 2.0f;
    float cy = sPanY + PORT_STEREO_EDITOR_VIEW_H / s / 2.0f;
    if (sSel == SEL_CELLS) {
        cx = (float)(RoomScreenX() + (sCol0 + sCol1 + 1) * 4);
        cy = (float)(RoomScreenY() + (sRow0 + sRow1 + 1) * 4);
    }
    sZoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
    sPanX = cx - PORT_STEREO_EDITOR_VIEW_W / Scale() / 2.0f;
    sPanY = cy - PORT_STEREO_EDITOR_VIEW_H / Scale() / 2.0f;
    ClampPan();
}

static void ResetSelection(void) {
    ++sSelectionRevision;
    memset(sSelection, 0, sizeof(sSelection));
    sSelected = 0;
    sCol0 = sRow0 = SIDE;
    sCol1 = sRow1 = -1;
    sArea = gRoomControls.area;
    sRoom = gRoomControls.room;
    sSel = SEL_NONE;
}

static void SelectCell(int col, int row) {
    if (!CellInRoom(col, row) || sSelection[row * SIDE + col]) {
        return;
    }
    sSelection[row * SIDE + col] = 1;
    ++sSelected;
    ++sSelectionRevision;
    sCol0 = Min(sCol0, col);
    sRow0 = Min(sRow0, row);
    sCol1 = Max(sCol1, col);
    sRow1 = Max(sRow1, row);
    sSel = SEL_CELLS;
}

static void SelectDrag(int touchX, int touchY) {
    const int c0 = CellCol(ToGbaX(sTouchX0)), r0 = CellRow(ToGbaY(sTouchY0));
    const int c1 = CellCol(ToGbaX(touchX)), r1 = CellRow(ToGbaY(touchY));
    ResetSelection();
    for (int row = Max(0, Min(r0, r1)); row <= Min(RoomRows() - 1, Max(r0, r1)); ++row) {
        for (int col = Max(0, Min(c0, c1)); col <= Min(RoomCols() - 1, Max(c0, c1)); ++col) {
            SelectCell(col, row);
        }
    }
}

/* The map tile under a cell on the layers being edited; two cells match when
 * every layer asked shows the same tile there. */
static u32 TileKey(int col, int row) {
    const u32 tile = (u32)(col >> 1) | ((u32)(row >> 1) << 6);
    u32 key = 0;
    if (sLayers & PORT_STEREO_EDIT_BOTTOM) {
        key |= gMapBottom.mapData[tile];
    }
    if (sLayers & PORT_STEREO_EDIT_TOP) {
        key |= (u32)gMapTop.mapData[tile] << 16;
    }
    return key;
}

static void SelectSameTiles(int col, int row) {
    const u32 key = TileKey(col, row);
    ResetSelection();
    for (int r = 0; r < RoomRows(); ++r) {
        for (int c = 0; c < RoomCols(); ++c) {
            if (TileKey(c, r) == key) {
                SelectCell(c, r);
            }
        }
    }
}

static void SelectTap(void) {
    Pick pick = { ToGbaX(sTouchX0), ToGbaY(sTouchY0), 0.0f, NULL };
    ForEachEntity(PickVisit, &pick);
    const bool soon = sFrame - sTapFrame <= DOUBLE_TAP_FRAMES;
    sTapFrame = sFrame;
    if (pick.found != NULL) {
        const Entity* e = pick.found;
        const bool again = soon && sSel == SEL_ENTITY && e == sTapEntity;
        sKey = (PortStereoEntityKey){ e->kind, e->id, e->type, FALSE, gRoomControls.area, gRoomControls.room,
                                      (u8)Max(0, EntityCol(e)), (u8)Max(0, EntityRow(e)) };
        sKey.all = again;
        sSel = SEL_ENTITY;
        sTapEntity = e;
        sTapCol = sTapRow = -1;
        return;
    }
    const int col = CellCol(pick.x), row = CellRow(pick.y);
    sTapEntity = NULL;
    if (!CellInRoom(col, row)) {
        ResetSelection();
        return;
    }
    if (soon && col == sTapCol && row == sTapRow) {
        SelectSameTiles(col, row);
    } else {
        ResetSelection();
        SelectCell(col, row);
    }
    sTapCol = col;
    sTapRow = row;
}

static bool OnHelpButton(int x, int y) {
    return x >= PORT_STEREO_EDITOR_HELP_X0 && y >= PORT_STEREO_EDITOR_HELP_Y0;
}

static bool OnRoomsButton(int x, int y) {
    return x >= PORT_STEREO_EDITOR_ROOMS_X0 && x < PORT_STEREO_EDITOR_HELP_X0 - 2 && y >= PORT_STEREO_EDITOR_HELP_Y0;
}

void PortStereoEditor_Input(uint32_t down, uint32_t held, bool touching, int touchX, int touchY, int padX,
                            int padY) {
    if (!sOpen) {
        return;
    }
    ++sFrame;
    if (sHelp) {
        /* The help covers the picture; B or a tap closes it. */
        if (sWaitRelease) {
            sWaitRelease = touching;
        } else if (touching) {
            sWaitRelease = true;
            sHelp = false;
        }
        if (down & PORT_STEREO_EDITOR_B) {
            sHelp = false;
        }
        PublishStatus();
        return;
    }
    if (sWaitRelease) {
        sWaitRelease = touching;
        touching = false;
    }
    if (ListInput(down, held, touching, touchX, touchY)) {
        if (touching) {
            sWaitRelease = true;
        }
        PublishStatus();
        return;
    }
    if (down & (PORT_STEREO_EDITOR_B | PORT_STEREO_EDITOR_START)) {
        sSaveFailed = !PortStereoEdits_Save();
        if (!sSaveFailed) {
            sOpen = false;
        }
        PublishStatus();
        return;
    }
    const bool live = InGame();
    if (down & PORT_STEREO_EDITOR_SELECT) {
        sHeatmap = !sHeatmap;
    }
    if (sSel == SEL_CELLS && (sArea != gRoomControls.area || sRoom != gRoomControls.room)) {
        ResetSelection();
    }

    if (down & PORT_STEREO_EDITOR_L) {
        ZoomTo(sZoom - 1);
    }
    if (down & PORT_STEREO_EDITOR_R) {
        ZoomTo(sZoom + 1);
    }
    if (abs(padX) > 20 || abs(padY) > 20) {
        sPanX += (float)padX / 52.0f / (float)sZoom;
        sPanY -= (float)padY / 52.0f / (float)sZoom;
        ClampPan();
    }

    if (touching && !sTouching && OnRoomsButton(touchX, touchY)) {
        OpenAreas();
        sWaitRelease = true;
        PublishStatus();
        return;
    } else if (touching && !sTouching && OnHelpButton(touchX, touchY)) {
        sHelp = true;
        sWaitRelease = true;
        PublishStatus();
        return;
    } else if (touching && (sTouching || touchY < PORT_STEREO_EDITOR_VIEW_H)) {
        if (!sTouching) {
            sTouching = true;
            sMoved = false;
            sTouchX0 = touchX;
            sTouchY0 = touchY;
        }
        if (abs(touchX - sTouchX0) > DRAG_SLOP || abs(touchY - sTouchY0) > DRAG_SLOP) {
            sMoved = true;
        }
        if (sMoved && live) {
            SelectDrag(touchX, Min(touchY, PORT_STEREO_EDITOR_VIEW_H - 1));
        }
    } else if (!touching && sTouching) {
        sTouching = false;
        if (!sMoved && live) {
            SelectTap();
        }
    }

    if (down & (PORT_STEREO_EDITOR_LEFT | PORT_STEREO_EDITOR_RIGHT)) {
        static const int kOrder[] = { PORT_STEREO_EDIT_BOTH, PORT_STEREO_EDIT_BOTTOM, PORT_STEREO_EDIT_TOP };
        int i = 0;
        while (kOrder[i] != sLayers) {
            ++i;
        }
        i = (i + ((down & PORT_STEREO_EDITOR_RIGHT) ? 1 : 2)) % 3;
        sLayers = kOrder[i];
    }
    if ((down & PORT_STEREO_EDITOR_Y) && sSel == SEL_ENTITY) {
        sKey.all = !sKey.all;
    }

    const uint32_t vertical = held & (PORT_STEREO_EDITOR_UP | PORT_STEREO_EDITOR_DOWN);
    if (!vertical) {
        sRepeat = 0;
    }
    int step = 0;
    if ((down & vertical) || (vertical && ++sRepeat > 20 && sRepeat % 6 == 0)) {
        step = (vertical & PORT_STEREO_EDITOR_UP) ? 1 : -1;
    }
    const bool clear = (down & PORT_STEREO_EDITOR_X) != 0;
    if (live && sSel == SEL_CELLS) {
        if (clear) {
            PortStereoEdits_Reset(sArea, sRoom, sLayers, sSelection);
        } else if (step != 0) {
            PortStereoEdits_Step(sArea, sRoom, sLayers, sSelection, step);
        } else if (down & PORT_STEREO_EDITOR_A) {
            /* Level the selection at its lowest point, each layer by itself. */
            for (int layer = PORT_STEREO_EDIT_BOTTOM; layer <= PORT_STEREO_EDIT_TOP; layer <<= 1) {
                int lo, hi;
                if ((sLayers & layer) && SelectionRange(layer, &lo, &hi)) {
                    PortStereoEdits_Set(sArea, sRoom, layer, sSelection, lo);
                }
            }
        }
    } else if (live && sSel == SEL_ENTITY && (step != 0 || clear)) {
        PortStereoEdits_AdjustEntity(&sKey, step, clear);
    }
    PublishStatus();
}

static uint32_t Color(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Ground at the reference level is left clear; lower is blue, higher runs
 * green, yellow, orange, red, purple. */
static uint32_t HeightColour(int height) {
    static const unsigned kUp[6][3] = { { 60, 220, 60 },  { 170, 230, 40 }, { 250, 220, 40 },
                                        { 250, 150, 30 }, { 240, 60, 40 },  { 200, 40, 170 } };
    if (height == 0) {
        return 0;
    }
    if (height < 0) {
        return Color(40, 90, 255, (unsigned)(70 + 25 * Min(-height, 4)));
    }
    const unsigned* c = kUp[Min(height, 6) - 1];
    return Color(c[0], c[1], c[2], 110);
}

static void AddRect(PortStereoEditorView* view, float x, float y, float w, float h, uint32_t abgr) {
    if (x < 0.0f) {
        w += x;
        x = 0.0f;
    }
    if (y < 0.0f) {
        h += y;
        y = 0.0f;
    }
    if (x + w > PORT_STEREO_EDITOR_VIEW_W) {
        w = PORT_STEREO_EDITOR_VIEW_W - x;
    }
    if (y + h > PORT_STEREO_EDITOR_VIEW_H) {
        h = PORT_STEREO_EDITOR_VIEW_H - y;
    }
    if (w <= 0.0f || h <= 0.0f || view->rectCount >= PORT_STEREO_EDITOR_MAX_RECTS) {
        return;
    }
    view->rects[view->rectCount++] = (PortStereoEditorRect){ x, y, w, h, abgr };
}

static void AddOutline(PortStereoEditorView* view, float x, float y, float w, float h, float t, uint32_t abgr) {
    AddRect(view, x - t, y - t, w + 2 * t, t, abgr);
    AddRect(view, x - t, y + h, w + 2 * t, t, abgr);
    AddRect(view, x - t, y, t, h, abgr);
    AddRect(view, x + w, y, t, h, abgr);
}

/* GBA screen pixels to bottom-screen pixels. */
static float ViewX(float gbaX) {
    return (gbaX - sPanX) * Scale();
}

static float ViewY(float gbaY) {
    return (gbaY - sPanY) * Scale();
}

typedef struct {
    PortStereoEditorView* view;
} Highlight;

static bool HighlightVisit(Entity* e, void* context) {
    Highlight* h = context;
    if (KeyMatches(&sKey, e)) {
        const float s = Scale();
        AddOutline(h->view, ViewX(EntityScreenX(e) - 8.0f), ViewY(EntityScreenY(e) - 20.0f), 16.0f * s, 22.0f * s,
                   1.0f, Color(80, 240, 255, 255));
    }
    return true;
}

void PortStereoEditor_BuildView(PortStereoEditorView* view) {
    view->rectCount = 0;
    view->cells = false;
    view->hidden = sHelp || sList != LIST_CLOSED;
    const float s = Scale();
    const float x0 = sPanX > 0.0f ? sPanX : 0.0f, y0 = sPanY > 0.0f ? sPanY : 0.0f;
    const float fw = (float)sFrameW, fh = (float)sFrameH;
    const float x1 = sPanX + PORT_STEREO_EDITOR_VIEW_W / s < fw ? sPanX + PORT_STEREO_EDITOR_VIEW_W / s : fw;
    const float y1 = sPanY + PORT_STEREO_EDITOR_VIEW_H / s < fh ? sPanY + PORT_STEREO_EDITOR_VIEW_H / s : fh;
    view->image = true;
    view->srcX = x0;
    view->srcY = y0;
    view->srcW = x1 - x0;
    view->srcH = y1 - y0;
    view->dstX = ViewX(x0);
    view->dstY = ViewY(y0);
    view->dstW = view->srcW * s;
    view->dstH = view->srcH * s;
    if (view->hidden || !InGame()) {
        return;
    }

    const int roomX = RoomScreenX(), roomY = RoomScreenY();
    const int col0 = CellCol(x0), col1 = CellCol(x1 - 1.0f), row0 = CellRow(y0), row1 = CellRow(y1 - 1.0f);
    const bool selectionHere = sSel == SEL_CELLS && sArea == gRoomControls.area && sRoom == gRoomControls.room;
    /* The colour layer: how high each cell stands, or only which carry an
     * edit when SELECT has turned the heights off; the selection framed. */
    view->cells = true;
    view->cellsX = (float)(roomX + col0 * 8);
    view->cellsY = (float)(roomY + row0 * 8);
    view->cellCols = Min(PORT_STEREO_EDITOR_CELLS, col1 - col0 + 1);
    view->cellRows = Min(PORT_STEREO_EDITOR_CELLS, row1 - row0 + 1);
    for (int r = 0; r < view->cellRows; ++r) {
        for (int c = 0; c < view->cellCols; ++c) {
            const int col = col0 + c, row = row0 + r, at = r * PORT_STEREO_EDITOR_CELLS + c;
            uint32_t colour = 0;
            bool edited = false, selected = false;
            if (col <= col1 && row <= row1 && CellInRoom(col, row)) {
                int h;
                edited = PortStereoEdits_CellEdited(gRoomControls.area, gRoomControls.room, col, row, sLayers) != 0;
                selected = selectionHere && sSelection[row * SIDE + col];
                if (sHeatmap && CellHeight(col, row, sLayers, &h)) {
                    colour = HeightColour(h);
                } else if (!sHeatmap && edited) {
                    colour = Color(255, 255, 255, 90);
                }
            }
            view->cellColour[at] = colour;
            view->cellEdited[at] = edited && sHeatmap;
            view->cellSelected[at] = selected;
        }
    }
    /* The grid of cells over the picture, every second line (a whole tile)
     * stronger. */
    for (int col = col0; col <= col1 + 1; ++col) {
        const float x = ViewX((float)(roomX + col * 8));
        if ((col & 1) && sZoom == 1) {
            continue; /* whole tiles only, or the cells crowd the picture */
        }
        if (x >= view->dstX && x <= view->dstX + view->dstW) {
            AddRect(view, x, view->dstY, 1.0f, view->dstH, (col & 1) ? Color(255, 255, 255, 70) : Color(0, 0, 0, 110));
        }
    }
    for (int row = row0; row <= row1 + 1; ++row) {
        const float y = ViewY((float)(roomY + row * 8));
        if ((row & 1) && sZoom == 1) {
            continue;
        }
        if (y >= view->dstY && y <= view->dstY + view->dstH) {
            AddRect(view, view->dstX, y, view->dstW, 1.0f, (row & 1) ? Color(255, 255, 255, 70) : Color(0, 0, 0, 110));
        }
    }
    if (sSel == SEL_ENTITY) {
        Highlight h = { view };
        ForEachEntity(HighlightVisit, &h);
    }
}

void PortStereoEditor_Status(char* line, size_t size) {
    snprintf(line, size, "%s", sLines[sLineBuffer]);
}

const char* const* PortStereoEditor_HelpLines(int* count) {
    static const char* const kLines[] = {
        "Редактор 3D",
        "Стилус: тягни - область, торкни - клітинка/об'єкт",
        "Двічі по клітинці - усі такі плитки кімнати",
        "Двічі по об'єкту чи Y-кнопка - усі такі об'єкти",
        "Хрест вгору/вниз - вище/нижче, вліво/вправо - шар",
        "A-кнопка - вирівняти до найнижчої",
        "X-кнопка - скинути правки виділеного",
        "L/R - масштаб, C-стік - прокрутка",
        "Select - кольори висот чи лише правки",
        "B-кнопка - зберегти й вийти; кнопка К - кімнати",
        "Колір: синій нижче, зелений-червоний вище",
    };
    *count = (int)(sizeof(kLines) / sizeof(kLines[0]));
    return kLines;
}

void PortStereoEditor_ClearSelection(int area, int room) {
    ResetSelection();
    sArea = area;
    sRoom = room;
}

void PortStereoEditor_SelectRun(int area, int room, int row, int col0, int col1) {
    if (sSel != SEL_CELLS || sArea != area || sRoom != room) {
        PortStereoEditor_ClearSelection(area, room);
    }
    for (int col = col0; col <= col1; ++col) {
        if (col >= 0 && row >= 0 && col < SIDE && row < SIDE && !sSelection[row * SIDE + col]) {
            sSelection[row * SIDE + col] = 1;
            ++sSelected;
            sCol0 = Min(sCol0, col);
            sRow0 = Min(sRow0, row);
            sCol1 = Max(sCol1, col);
            sRow1 = Max(sRow1, row);
            sSel = SEL_CELLS;
        }
    }
    /* Not a change of ours: the PC editor already knows it. */
    if (sOpen) {
        PublishStatus();
    }
}

unsigned PortStereoEditor_SelectionRevision(void) {
    return sSelectionRevision;
}

char* PortStereoEditor_SelectionText(size_t* length) {
    const size_t capacity = 32 + (size_t)SIDE * 64 * 14;
    char* text = malloc(capacity);
    if (text == NULL) {
        return NULL;
    }
    size_t n = (size_t)snprintf(text, capacity, "%d %d\n", sArea, sRoom);
    if (sSel == SEL_CELLS) {
        for (int row = sRow0; row <= sRow1 && row < SIDE; ++row) {
            for (int col = sCol0; col <= sCol1 && col < SIDE;) {
                if (!sSelection[row * SIDE + col]) {
                    ++col;
                    continue;
                }
                int end = col;
                while (end + 1 <= sCol1 && sSelection[row * SIDE + end + 1]) {
                    ++end;
                }
                if (n + 16 < capacity) {
                    n += (size_t)snprintf(text + n, capacity - n, "%d %d %d\n", row, col, end);
                }
                col = end + 1;
            }
        }
    }
    *length = n;
    return text;
}

/* ---- The room list ---- */

static void PublishList(void) {
    const int next = !sListBuffer;
    int shown = 0;
    if (sList == LIST_AREAS) {
        snprintf(sListTitle[next], sizeof(sListTitle[next]), "Області (B - назад)");
    } else if (sList == LIST_ROOMS) {
        snprintf(sListTitle[next], sizeof(sListTitle[next]), "%02X %s", sListArea,
                 sListArea < AREA_NAME_COUNT && kAreaNames[sListArea] ? kAreaNames[sListArea] : "");
    }
    for (int i = 0; i < PORT_STEREO_EDITOR_LIST_ROWS && sListTop + i < sListCount; ++i) {
        const int item = sListItems[sListTop + i];
        char* line = sListLines[next][i];
        if (sList == LIST_AREAS) {
            snprintf(line, 48, "%02X  %s", item,
                     item < AREA_NAME_COUNT && kAreaNames[item] ? kAreaNames[item] : "");
        } else {
            int w = 0, h = 0;
            PortStereoLink_RoomSize(sListArea, item, &w, &h);
            const bool here = sListArea == gRoomControls.area && item == gRoomControls.room;
            snprintf(line, 48, "%02X:%02X  %dx%d%s", sListArea, item, w, h, here ? "  тут" : "");
        }
        ++shown;
    }
    sListShown[next] = sList == LIST_CLOSED ? 0 : shown;
    sListCursorShown[next] = sListCursor - sListTop;
    sListTopShown[next] = sListTop;
    sListCountShown[next] = sListCount;
    sListBuffer = next;
}

static void OpenAreas(void) {
    sList = LIST_AREAS;
    sListCount = 0;
    sListCursor = 0;
    for (int area = 0; area < 0x90 && sListCount < MAX_LIST; ++area) {
        int w, h;
        if (PortStereoLink_RoomSize(area, 0, &w, &h) || PortStereoLink_RoomSize(area, 1, &w, &h)) {
            if (area == gRoomControls.area) {
                sListCursor = sListCount;
            }
            sListItems[sListCount++] = area;
        }
    }
    sListTop = Max(0, sListCursor - PORT_STEREO_EDITOR_LIST_ROWS / 2);
    PublishList();
}

static void OpenRooms(int area) {
    sList = LIST_ROOMS;
    sListArea = area;
    sListCount = 0;
    sListCursor = 0;
    for (int room = 0; room < 64 && sListCount < MAX_LIST; ++room) {
        int w, h;
        if (PortStereoLink_RoomSize(area, room, &w, &h)) {
            if (area == gRoomControls.area && room == gRoomControls.room) {
                sListCursor = sListCount;
            }
            sListItems[sListCount++] = room;
        }
    }
    sListTop = Max(0, sListCursor - PORT_STEREO_EDITOR_LIST_ROWS / 2);
    PublishList();
}

static void ListChoose(int index) {
    if (index < 0 || index >= sListCount) {
        return;
    }
    sListCursor = index;
    if (sList == LIST_AREAS) {
        OpenRooms(sListItems[index]);
    } else if (PortStereoLink_Goto(sListArea, sListItems[index], -1, -1, 1) == 200) {
        sList = LIST_CLOSED;
        ResetSelection();
        PublishList();
    }
}

static void ListMove(int delta) {
    sListCursor = Max(0, Min(sListCount - 1, sListCursor + delta));
    if (sListCursor < sListTop) {
        sListTop = sListCursor;
    } else if (sListCursor >= sListTop + PORT_STEREO_EDITOR_LIST_ROWS) {
        sListTop = sListCursor - PORT_STEREO_EDITOR_LIST_ROWS + 1;
    }
    PublishList();
}

/* Scrolls so that the scroll bar's thumb is centred at bottom-screen row y,
 * keeping the highlighted line among those shown. */
static void ListScrollTo(int y) {
    const int rows = PORT_STEREO_EDITOR_LIST_ROWS;
    const int span = rows * PORT_STEREO_EDITOR_LIST_ROW_H;
    if (sListCount <= rows) {
        return;
    }
    const int at = (y - PORT_STEREO_EDITOR_LIST_Y0) * sListCount / span;
    sListTop = Max(0, Min(sListCount - rows, at - rows / 2));
    sListCursor = Max(sListTop, Min(sListTop + rows - 1, sListCursor));
    PublishList();
}

static void ListClose(void) {
    sList = LIST_CLOSED;
    PublishList();
}

/* Input while the list is up; true when it took the frame. */
static bool ListInput(uint32_t down, uint32_t held, bool touching, int touchX, int touchY) {
    static unsigned repeat;
    static bool wasTouching, dragging;
    if (sList == LIST_CLOSED) {
        wasTouching = touching;
        dragging = false;
        return false;
    }
    /* The "К" button that opened the list closes it again. */
    if (touching && !wasTouching && OnRoomsButton(touchX, touchY)) {
        wasTouching = true;
        dragging = false;
        ListClose();
        return true;
    }
    if (down & PORT_STEREO_EDITOR_B) {
        if (sList == LIST_ROOMS) {
            OpenAreas();
        } else {
            sList = LIST_CLOSED;
            PublishList();
        }
        return true;
    }
    const uint32_t vertical = held & (PORT_STEREO_EDITOR_UP | PORT_STEREO_EDITOR_DOWN);
    if (!vertical) {
        repeat = 0;
    }
    if ((down & vertical) || (vertical && ++repeat > 18 && repeat % 4 == 0)) {
        ListMove((vertical & PORT_STEREO_EDITOR_UP) ? -1 : 1);
    }
    if (down & PORT_STEREO_EDITOR_LEFT) {
        ListMove(-PORT_STEREO_EDITOR_LIST_ROWS);
    }
    if (down & PORT_STEREO_EDITOR_RIGHT) {
        ListMove(PORT_STEREO_EDITOR_LIST_ROWS);
    }
    if (down & PORT_STEREO_EDITOR_A) {
        ListChoose(sListCursor);
    }
    if (!touching) {
        dragging = false;
    } else if (dragging) {
        ListScrollTo(touchY);
    }
    if (touching && !wasTouching && touchX >= PORT_STEREO_EDITOR_SCROLL_X0 &&
        touchY >= PORT_STEREO_EDITOR_LIST_Y0 && touchY < PORT_STEREO_EDITOR_VIEW_H) {
        /* The scroll bar: drag it. */
        dragging = true;
        ListScrollTo(touchY);
    } else if (touching && !wasTouching && touchY < PORT_STEREO_EDITOR_VIEW_H) {
        const int row = (touchY - PORT_STEREO_EDITOR_LIST_Y0) / PORT_STEREO_EDITOR_LIST_ROW_H;
        if (touchY < PORT_STEREO_EDITOR_LIST_Y0) {
            /* The title: back. */
            if (sList == LIST_ROOMS) {
                OpenAreas();
            } else {
                sList = LIST_CLOSED;
                PublishList();
            }
        } else if (row >= 0 && row < PORT_STEREO_EDITOR_LIST_ROWS) {
            ListChoose(sListTop + row);
        }
    }
    wasTouching = touching;
    return true;
}

int PortStereoEditor_ListLines(char (*lines)[48], int* cursor, char* title, size_t titleSize) {
    const int current = sListBuffer;
    const int shown = sOpen ? sListShown[current] : 0;
    for (int i = 0; i < shown; ++i) {
        memcpy(lines[i], sListLines[current][i], 48);
    }
    *cursor = sListCursorShown[current];
    snprintf(title, titleSize, "%s", sListTitle[current]);
    return shown;
}

void PortStereoEditor_ListScroll(int* top, int* count) {
    const int current = sListBuffer;
    *top = sListTopShown[current];
    *count = sListCountShown[current];
}
