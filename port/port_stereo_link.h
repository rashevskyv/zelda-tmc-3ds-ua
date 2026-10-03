/**
 * @file port_stereo_link.h
 * @brief Stereoscopic 3D: the game as a small HTTP server for the PC editor.
 *
 * Developer tools > PC EDITOR (or `stereo_link=1` in the ini) opens port
 * PORT_STEREO_LINK_PORT. The PC editor -- a page in a browser -- asks it for
 * the room being played (tiles, graphics, collision, measured heights,
 * entities), sends Link to other rooms, replaces the corrections and sees
 * them at once in 3D, and points at cells, which the top screen then frames.
 *
 *   GET  /status                     where the game is, as JSON
 *   GET  /rooms                      every room of every area, as JSON
 *   GET  /room                       the current room, binary (see the editor)
 *   GET  /entities                   the current room's entities, as JSON
 *   GET  /edits?kinds=&area=&room=   corrections as text (kinds: 1 cells of
 *                                    that room, 2 tile rules, 4 entities)
 *   POST /edits?kinds=&area=&room=   replace those corrections by the body
 *   POST /goto?area=&room=&x=&y=     send Link there (x, y in the room)
 *   POST /select?area=&room=         frame cells on the top screen; body
 *                                    "row col0 col1" lines, empty to clear
 *   POST /save                       write stereo_edits.txt
 *
 * Every answer allows any origin, so the page can be opened from a file.
 */
#ifndef PORT_STEREO_LINK_H
#define PORT_STEREO_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_STEREO_LINK_PORT 8333

/* Once a frame on the game thread: accepts, reads, answers. */
void PortStereoLink_Tick(void);
void PortStereoLink_SetEnabled(bool enabled);
bool PortStereoLink_Enabled(void);
/* "OFF", "WAIT", "NO WI-FI" or the console's address; any thread. */
void PortStereoLink_Label(char* out, size_t size);

/* The cells the PC editor points at, as GBA screen rectangles {x, y, w, h};
 * returns how many. Main thread. */
int PortStereoLink_Highlight(float (*rects)[4], int max);

/* Platform: bring the network up (returns the address, host order) or down. */
bool PortStereoLink_NetUp(uint32_t* address);
void PortStereoLink_NetDown(void);

#endif /* PORT_STEREO_LINK_H */
