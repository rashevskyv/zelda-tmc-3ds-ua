/**
 * @file port_stereo_editor.c
 * @brief Stereoscopic 3D editor: correct the relief by hand on the touch screen.
 *
 * See port_stereo_editor.h. Everything here runs on the main thread between
 * frames, where the room and the entity lists are the engine's own; only the
 * help lines are read from the bottom-screen painter's thread, through a
 * double buffer.
 */
#include "port_stereo_editor.h"
#include "port_stereo.h"
#include "port_stereo_edits.h"

#include "global.h"
#include "entity.h"
#include "main.h"
#include "room.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whole 240x160 frame fits the 320x200 view at 1.25. */
#define BASE_SCALE 1.25f
enum { MAX_ZOOM = 4, DRAG_SLOP = 3, LINE_SIZE = 160 };
enum { SEL_NONE, SEL_RECT, SEL_ENTITY };

static volatile bool sOpen;
static int sZoom = 1;
static float sPanX, sPanY;
static int sLayers = PORT_STEREO_EDIT_BOTH;
static int sSel;
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
/* A second tap on the same sprite soon after the first picks all its kind. */
static unsigned sFrame, sTapFrame;
static const Entity* sTapEntity;
enum { DOUBLE_TAP_FRAMES = 30 };
static bool sSaveFailed;

static char sLines[2][2][LINE_SIZE];
static volatile int sLineBuffer;

static float Scale(void) {
    return BASE_SCALE * (float)sZoom;
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
    sPanX = w >= 240.0f ? (240.0f - w) / 2.0f : ClampF(sPanX, 0.0f, 240.0f - w);
    sPanY = h >= 160.0f ? (160.0f - h) / 2.0f : ClampF(sPanY, 0.0f, 160.0f - h);
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

static bool CellInRoom(int col, int row) {
    return col >= 0 && row >= 0 && col < gRoomControls.width / 8 && row < gRoomControls.height / 8;
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
    return layers == PORT_STEREO_EDIT_BOTTOM ? "нижній шар" : layers == PORT_STEREO_EDIT_TOP ? "верхній шар" : "обидва шари";
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

/* The lowest and highest the selection stands, for the layers being edited. */
static bool SelectionRange(int layers, int* lo, int* hi) {
    bool any = false;
    for (int row = sRow0; row <= sRow1; ++row) {
        for (int col = sCol0; col <= sCol1; ++col) {
            int h;
            if (CellHeight(col, row, layers, &h)) {
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
    char* line1 = sLines[next][0];
    char* line2 = sLines[next][1];
    if (!InGame()) {
        snprintf(line1, LINE_SIZE, "Редактор 3D працює лише в грі");
    } else if (!Port_Stereo_ReliefLive()) {
        snprintf(line1, LINE_SIZE, "Підніми повзунок 3D; РЕЛЬЄФ 3D увімкнено");
    } else if (sSel == SEL_RECT) {
        int lo = 0, hi = 0;
        SelectionRange(sLayers, &lo, &hi);
        if (lo == hi) {
            snprintf(line1, LINE_SIZE, "%02X:%02X  %d,%d-%d,%d  %s  висота %d", sArea, sRoom, sCol0, sRow0, sCol1,
                     sRow1, LayerName(sLayers), lo);
        } else {
            snprintf(line1, LINE_SIZE, "%02X:%02X  %d,%d-%d,%d  %s  висота %d..%d", sArea, sRoom, sCol0, sRow0,
                     sCol1, sRow1, LayerName(sLayers), lo, hi);
        }
    } else if (sSel == SEL_ENTITY) {
        snprintf(line1, LINE_SIZE, "Об'єкт %02X %02X %02X  %s  %+d", sKey.kind, sKey.id, sKey.type,
                 sKey.all ? "усі такі" : "цей", PortStereoEdits_KeyDelta(&sKey));
    } else {
        snprintf(line1, LINE_SIZE, "%02X:%02X  тягни стилусом або торкнись об'єкта", gRoomControls.area,
                 gRoomControls.room);
    }
    snprintf(line2, LINE_SIZE, "%s",
             sSaveFailed       ? "Не вдалося зберегти stereo_edits.txt"
             : sSel == SEL_RECT ? "Хрест-висота/шар  A-рівно  X-скинути  L/R-зум  B-вихід"
             : sSel == SEL_ENTITY
                 ? "Хрест-ближче/далі  2 дотики чи Y-усі  X-скинути  B-вихід"
                 : "Тягни чи торкнись  L/R-зум  Select-кольори  B-вихід");
    sLineBuffer = next;
}

void PortStereoEditor_Open(void) {
    PortStereoEdits_Load();
    sSel = SEL_NONE;
    sTouching = false;
    sWaitRelease = true;
    sRepeat = 0;
    sSaveFailed = false;
    ClampPan();
    PublishStatus();
    sOpen = true;
}

bool PortStereoEditor_IsOpen(void) {
    return sOpen;
}

/* Zooms about the selection, or the middle of the view. */
static void ZoomTo(int zoom) {
    const float s = Scale();
    float cx = sPanX + PORT_STEREO_EDITOR_VIEW_W / s / 2.0f;
    float cy = sPanY + PORT_STEREO_EDITOR_VIEW_H / s / 2.0f;
    if (sSel == SEL_RECT) {
        cx = (float)(RoomScreenX() + (sCol0 + sCol1 + 1) * 4);
        cy = (float)(RoomScreenY() + (sRow0 + sRow1 + 1) * 4);
    }
    sZoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
    sPanX = cx - PORT_STEREO_EDITOR_VIEW_W / Scale() / 2.0f;
    sPanY = cy - PORT_STEREO_EDITOR_VIEW_H / Scale() / 2.0f;
    ClampPan();
}

static void SelectDrag(int touchX, int touchY) {
    const int c0 = CellCol(ToGbaX(sTouchX0)), r0 = CellRow(ToGbaY(sTouchY0));
    const int c1 = CellCol(ToGbaX(touchX)), r1 = CellRow(ToGbaY(touchY));
    const int maxCol = gRoomControls.width / 8 - 1, maxRow = gRoomControls.height / 8 - 1;
    sCol0 = Max(0, Min(c0, c1));
    sRow0 = Max(0, Min(r0, r1));
    sCol1 = Min(maxCol, Max(c0, c1));
    sRow1 = Min(maxRow, Max(r0, r1));
    sArea = gRoomControls.area;
    sRoom = gRoomControls.room;
    sSel = sCol0 <= sCol1 && sRow0 <= sRow1 ? SEL_RECT : SEL_NONE;
}

static void SelectTap(void) {
    Pick pick = { ToGbaX(sTouchX0), ToGbaY(sTouchY0), 0.0f, NULL };
    ForEachEntity(PickVisit, &pick);
    if (pick.found != NULL) {
        const Entity* e = pick.found;
        const bool again = sSel == SEL_ENTITY && e == sTapEntity && sFrame - sTapFrame <= DOUBLE_TAP_FRAMES;
        sKey = (PortStereoEntityKey){ e->kind, e->id, e->type, FALSE, gRoomControls.area, gRoomControls.room,
                                      (u8)Max(0, EntityCol(e)), (u8)Max(0, EntityRow(e)) };
        sKey.all = again;
        sSel = SEL_ENTITY;
        sTapEntity = e;
        sTapFrame = sFrame;
        return;
    }
    SelectDrag(sTouchX0, sTouchY0);
    if (!CellInRoom(sCol0, sRow0)) {
        sSel = SEL_NONE;
    }
}

void PortStereoEditor_Input(uint32_t down, uint32_t held, bool touching, int touchX, int touchY, int padX,
                            int padY) {
    if (!sOpen) {
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
    ++sFrame;
    const bool live = InGame();
    if (down & PORT_STEREO_EDITOR_SELECT) {
        sHeatmap = !sHeatmap;
    }
    if (sSel != SEL_NONE && sSel != SEL_ENTITY && (sArea != gRoomControls.area || sRoom != gRoomControls.room)) {
        sSel = SEL_NONE;
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

    if (sWaitRelease) {
        sWaitRelease = touching;
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
    if (live && sSel == SEL_RECT) {
        if (clear) {
            PortStereoEdits_Reset(sArea, sRoom, sLayers, sCol0, sRow0, sCol1, sRow1);
        } else if (step != 0) {
            PortStereoEdits_Step(sArea, sRoom, sLayers, sCol0, sRow0, sCol1, sRow1, step);
        } else if (down & PORT_STEREO_EDITOR_A) {
            /* Level the selection at its lowest point, each layer by itself. */
            for (int layer = PORT_STEREO_EDIT_BOTTOM; layer <= PORT_STEREO_EDIT_TOP; layer <<= 1) {
                int lo, hi;
                if ((sLayers & layer) && SelectionRange(layer, &lo, &hi)) {
                    PortStereoEdits_Set(sArea, sRoom, layer, sCol0, sRow0, sCol1, sRow1, lo);
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
    const float s = Scale();
    const float x0 = sPanX > 0.0f ? sPanX : 0.0f, y0 = sPanY > 0.0f ? sPanY : 0.0f;
    const float x1 = sPanX + PORT_STEREO_EDITOR_VIEW_W / s < 240.0f ? sPanX + PORT_STEREO_EDITOR_VIEW_W / s : 240.0f;
    const float y1 = sPanY + PORT_STEREO_EDITOR_VIEW_H / s < 160.0f ? sPanY + PORT_STEREO_EDITOR_VIEW_H / s : 160.0f;
    view->image = true;
    view->srcX = x0;
    view->srcY = y0;
    view->srcW = x1 - x0;
    view->srcH = y1 - y0;
    view->dstX = ViewX(x0);
    view->dstY = ViewY(y0);
    view->dstW = view->srcW * s;
    view->dstH = view->srcH * s;
    if (!InGame()) {
        return;
    }

    const int roomX = RoomScreenX(), roomY = RoomScreenY();
    const int col0 = CellCol(x0), col1 = CellCol(x1 - 1.0f), row0 = CellRow(y0), row1 = CellRow(y1 - 1.0f);
    /* The colour layer: how high each cell stands, or only which carry an
     * edit when SELECT has turned the heights off. */
    view->cells = true;
    view->cellsX = (float)(roomX + col0 * 8);
    view->cellsY = (float)(roomY + row0 * 8);
    for (int r = 0; r < PORT_STEREO_EDITOR_CELLS; ++r) {
        for (int c = 0; c < PORT_STEREO_EDITOR_CELLS; ++c) {
            const int col = col0 + c, row = row0 + r, at = r * PORT_STEREO_EDITOR_CELLS + c;
            uint32_t colour = 0;
            bool edited = false;
            if (col <= col1 && row <= row1 && CellInRoom(col, row)) {
                int h;
                edited = PortStereoEdits_CellEdited(gRoomControls.area, gRoomControls.room, col, row, sLayers) != 0;
                if (sHeatmap && CellHeight(col, row, sLayers, &h)) {
                    colour = HeightColour(h);
                } else if (!sHeatmap && edited) {
                    colour = Color(255, 255, 255, 90);
                }
            }
            view->cellColour[at] = colour;
            view->cellEdited[at] = edited && sHeatmap;
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
    if (sSel == SEL_RECT && sArea == gRoomControls.area && sRoom == gRoomControls.room) {
        AddOutline(view, ViewX((float)(roomX + sCol0 * 8)), ViewY((float)(roomY + sRow0 * 8)),
                   (float)((sCol1 - sCol0 + 1) * 8) * s, (float)((sRow1 - sRow0 + 1) * 8) * s, 2.0f,
                   Color(255, 230, 40, 255));
    } else if (sSel == SEL_ENTITY) {
        Highlight h = { view };
        ForEachEntity(HighlightVisit, &h);
    }
}

void PortStereoEditor_Status(char* line1, char* line2, size_t size) {
    const int current = sLineBuffer;
    snprintf(line1, size, "%s", sLines[current][0]);
    snprintf(line2, size, "%s", sLines[current][1]);
}
