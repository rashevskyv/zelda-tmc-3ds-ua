/**
 * @file port_stereo_editor.h
 * @brief Stereoscopic 3D editor: correct the relief by hand on the touch screen.
 *
 * Opened from the developer tools. The top screen goes on in 3D; the bottom
 * one shows the same picture, flat, with the room's 8x8 cells -- the pieces
 * the relief is raised in -- and a line of help. Drag with the stylus to
 * select a rectangle of cells, tap to pick a sprite; the D-pad raises or
 * lowers what is selected. The game keeps running but gets no input.
 * Corrections go to port_stereo_edits.c, which saves them on closing.
 *
 * Platform code feeds input, draws the view this file describes, and paints
 * the help line from PortStereoEditor_Status.
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
};

/* The part of the bottom screen the picture takes; the help line is below. */
enum {
    PORT_STEREO_EDITOR_VIEW_W = 320,
    PORT_STEREO_EDITOR_VIEW_H = 200,
    /* citro2d draws at most 320 objects a frame, both screens together. */
    PORT_STEREO_EDITOR_MAX_RECTS = 280,
};

typedef struct PortStereoEditorRect {
    float x, y, w, h;
    uint32_t abgr; /* as C2D_Color32 packs it */
} PortStereoEditorRect;

typedef struct PortStereoEditorView {
    /* The game picture: a GBA-pixel rectangle of the 240x160 frame and where
     * on the bottom screen it goes. */
    bool image;
    float srcX, srcY, srcW, srcH;
    float dstX, dstY, dstW, dstH;
    int rectCount;
    PortStereoEditorRect rects[PORT_STEREO_EDITOR_MAX_RECTS];
} PortStereoEditorView;

void PortStereoEditor_Open(void);
bool PortStereoEditor_IsOpen(void);

/* One frame of input while open: buttons newly pressed and held, the stylus
 * (bottom-screen pixels) and the Circle Pad (-156..156). Main thread. */
void PortStereoEditor_Input(uint32_t down, uint32_t held, bool touching, int touchX, int touchY, int padX,
                            int padY);

/* What to draw over the bottom screen this frame. Main thread. */
void PortStereoEditor_BuildView(PortStereoEditorView* view);

/* The two help lines, UTF-8; any thread. */
void PortStereoEditor_Status(char* line1, char* line2, size_t size);

#endif /* PORT_STEREO_EDITOR_H */
