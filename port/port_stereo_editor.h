/**
 * @file port_stereo_editor.h
 * @brief Stereoscopic 3D editor: correct the relief by hand on the touch screen.
 *
 * Opened from the developer tools. The top screen goes on in 3D; the bottom
 * one shows the same picture, flat, with the room's 8x8 cells -- the pieces
 * the relief is raised in -- and a line of help. Drag with the stylus to
 * select a rectangle of cells, tap to pick a sprite; the D-pad raises or
 * lowers what is selected. The game keeps running with only the Circle Pad.
 * Corrections go to port_stereo_edits.c, which saves them on closing.
 *
 * Platform code feeds input, draws the view this file describes, and paints
 * the status line and the help from PortStereoEditor_Status / _HelpLines.
 */
#ifndef PORT_STEREO_EDITOR_H
#define PORT_STEREO_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PORT_STEREO_EDITOR_UP = 1 << 0,
    PORT_STEREO_EDITOR_DOWN = 1 << 1,
    PORT_STEREO_EDITOR_LEFT = 1 << 2,
    PORT_STEREO_EDITOR_RIGHT = 1 << 3,
    PORT_STEREO_EDITOR_A = 1 << 4,
    PORT_STEREO_EDITOR_B = 1 << 5,
    PORT_STEREO_EDITOR_X = 1 << 6,
    PORT_STEREO_EDITOR_Y = 1 << 7,
    PORT_STEREO_EDITOR_L = 1 << 8,
    PORT_STEREO_EDITOR_R = 1 << 9,
    PORT_STEREO_EDITOR_START = 1 << 10,
    PORT_STEREO_EDITOR_SELECT = 1 << 11,
};

/* The part of the bottom screen the picture takes; the help line is below. */
enum {
    PORT_STEREO_EDITOR_VIEW_W = 320,
    /* ...two text lines below it: what the buttons do, then the status. */
    PORT_STEREO_EDITOR_VIEW_H = 184,
    /* citro2d draws at most 320 objects a frame, both screens together. */
    PORT_STEREO_EDITOR_MAX_RECTS = 280,
    /* Cells a side of the colour layer: a 266-pixel WIDE frame shows up to
     * 35 columns, a 400x240 one 51x31. */
    PORT_STEREO_EDITOR_CELLS = 64,
    /* ...and rows: a 400x240 frame shows 31. */
    PORT_STEREO_EDITOR_CELL_ROWS = 32,
    /* The "?" button in the corner of the status line, the room list's
     * button left of it. */
    PORT_STEREO_EDITOR_HELP_X0 = 290,
    PORT_STEREO_EDITOR_ROOMS_X0 = 260,
    /* "Г": the buttons go to the game (menus, the title) until tapped again. */
    PORT_STEREO_EDITOR_GAME_X0 = 230,
    /* "Р": the selection frame on the top screen, hidden or shown. */
    PORT_STEREO_EDITOR_FRAME_X0 = 200,
    PORT_STEREO_EDITOR_HELP_Y0 = 202,
};

typedef struct PortStereoEditorRect {
    float x, y, w, h;
    uint32_t abgr; /* as C2D_Color32 packs it */
} PortStereoEditorRect;

typedef struct PortStereoEditorView {
    /* The help is up: the panel paints it where the picture goes. */
    bool hidden;
    /* The game picture: a GBA-pixel rectangle of the frame and where
     * on the bottom screen it goes. */
    bool image;
    float srcX, srcY, srcW, srcH;
    float dstX, dstY, dstW, dstH;
    /* A colour per cell (0 for none), and which cells carry an edit, from the
     * cell whose top-left is at GBA pixel (cellsX, cellsY); drawn over the
     * picture, under the rectangles. */
    bool cells;
    float cellsX, cellsY;
    int cellCols, cellRows; /* how many of them are in use */
    uint32_t cellColour[PORT_STEREO_EDITOR_CELLS * PORT_STEREO_EDITOR_CELLS];
    bool cellEdited[PORT_STEREO_EDITOR_CELLS * PORT_STEREO_EDITOR_CELLS];
    bool cellSelected[PORT_STEREO_EDITOR_CELLS * PORT_STEREO_EDITOR_CELLS];
    /* Heights written in the cells (SELECT: colours, numbers, both, edits),
     * and bottom-screen pixels per GBA pixel to place them. */
    bool numbers;
    float scale;
    int8_t cellNumber[PORT_STEREO_EDITOR_CELLS * PORT_STEREO_EDITOR_CELLS];
    bool cellHasNumber[PORT_STEREO_EDITOR_CELLS * PORT_STEREO_EDITOR_CELLS];
    int rectCount;
    PortStereoEditorRect rects[PORT_STEREO_EDITOR_MAX_RECTS];
} PortStereoEditorView;

void PortStereoEditor_Open(void);
/* The size of the game's frame the renderer draws (240x160, wider in WIDE);
 * its texture's texel (0, 0) is the frame's top-left. Main thread. */
void PortStereoEditor_SetFrame(int width, int height);
bool PortStereoEditor_IsOpen(void);

/* One frame of input while open: buttons newly pressed and held, the stylus
 * (bottom-screen pixels) and the stick that scrolls a zoomed view (the C-stick,
 * -156..156; the Circle Pad stays with the game). Main thread. */
void PortStereoEditor_Input(uint32_t down, uint32_t held, bool touching, int touchX, int touchY, int padX,
                            int padY);

/* The selection, shared with the PC editor: cells it selects become the
 * editor's, and it reads the editor's back. Main thread. */
void PortStereoEditor_ClearSelection(int area, int room);
void PortStereoEditor_SelectRun(int area, int room, int row, int col0, int col1);
unsigned PortStereoEditor_SelectionRevision(void);
/* Cells selected now (-1: a sprite is). */
int PortStereoEditor_SelectedCount(void);
/* The editor's geometry as JSON, for GET /editor. */
void PortStereoEditor_Geometry(char* out, size_t size);
char* PortStereoEditor_SelectionText(size_t* length);

/* The room list (the "К" button, which also closes it): up to PORT_STEREO_EDITOR_LIST_ROWS lines
 * to show in the picture's place, the highlighted one, and a title; 0 when
 * the list is closed. Any thread. */
enum { PORT_STEREO_EDITOR_LIST_ROWS = 9, PORT_STEREO_EDITOR_LIST_Y0 = 22, PORT_STEREO_EDITOR_LIST_ROW_H = 17 };
int PortStereoEditor_ListLines(char (*lines)[48], int* cursor, char* title, size_t titleSize);
/* The list's scroll bar at the right edge, from LIST_Y0 down LIST_ROWS rows:
 * the first line shown and how many there are. Any thread. */
enum { PORT_STEREO_EDITOR_SCROLL_X0 = 302 };
void PortStereoEditor_ListScroll(int* top, int* count);

/* What to draw over the bottom screen this frame. Main thread. */
void PortStereoEditor_BuildView(PortStereoEditorView* view);

/* The status line (room, selection, height), UTF-8; any thread. */
void PortStereoEditor_Status(char* line, size_t size);
/* What the buttons do right now, for the line above the status. */
void PortStereoEditor_Hints(char* line, size_t size);
/* Whether the help is shown, and its lines; any thread. */
bool PortStereoEditor_HelpOpen(void);
void PortStereoEditor_ShowHelp(void);
void PortStereoEditor_ShowRooms(void);
/* The buttons go to the game, not the editor ("Г"); any thread. */
bool PortStereoEditor_GameKeys(void);
const char* const* PortStereoEditor_HelpLines(int* count);

#endif /* PORT_STEREO_EDITOR_H */
