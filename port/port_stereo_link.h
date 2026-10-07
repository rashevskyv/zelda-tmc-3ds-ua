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
 *   GET  /                           the PC editor page itself (from romfs)
 *   GET  /status                     where the game is, as JSON
 *   GET  /rooms                      every room of every area, as JSON
 *   GET  /room                       the current room, binary (see the editor)
 *   GET  /entities                   the current room's entities, as JSON
 *   GET  /cell?col=&row=             a cell's heights as the relief uses them
 *   GET  /selection                  the console's 3D editor selection
 *                                    ("area room", then "row col0 col1")
 *   GET  /bottom                     the bottom screen's panel image: "TMCB",
 *                                    u16 320, u16 240, ABGR words
 *   GET  /frame                      both eyes as the console draws them:
 *                                    "TMCF", u16 240, u16 160, then left and
 *                                    right, RGBA5551 rows
 *   GET  /edits?kinds=&area=&room=   corrections as text (kinds: 1 cells of
 *                                    that room, 2 tile rules, 4 entities)
 *   POST /edits?kinds=&area=&room=   replace those corrections by the body
 *   POST /goto?area=&room=&x=&y=     send Link there (x, y in the room)
 *   POST /select?area=&room=         frame cells on the top screen; body
 *                                    "row col0 col1" lines, empty to clear
 *   POST /save                       write stereo_edits.txt
 *   POST /file?name=x.3dsx[&quit=0]  a new build into sdmc:/3ds/ (3DSX only); the
 *                                    game then quits to the Homebrew Launcher
 *   POST /sweep                      tour the room with the camera (Link stays)
 *                                    so /room carries every sprite of it
 *   POST /remove?name=x.3dsx         delete an old build from sdmc:/3ds/
 *   POST /test?on=0|1&noclip=0|1     test mode: every item, full hearts, no
 *                                    saves written; ends (and restores the
 *                                    save) when switched off or the link goes
 *   POST /goto before a save is loaded starts the game from the last save
 *   (title: START; file select: that slot, or a new game) and then warps.
 *   POST /relaunch?name=x.3dsx       quit and start that build (from the
 *                                    Homebrew Launcher only)
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
/* Left by a relaunch for the build that starts next (current directory). */
#define PORT_STEREO_LINK_RELAUNCH_MARKER "stereo_link.relaunch"

/* While a build is coming in: the game holds still and the bottom screen
 * shows how far it is (0..1000 per mille), or -1 when nothing comes. */
int PortStereoLink_UploadProgress(void);
/* The build coming in (bytes so far and in all), and the name of one just
 * written while its note is up; any thread. */
bool PortStereoLink_UploadInfo(unsigned* received, unsigned* total, const char** doneName);
/* Seconds until the game quits after a build came in, or -1. */
int PortStereoLink_QuitSeconds(void);

/* Close the sockets and the network: on leaving the game. */
void PortStereoLink_Shutdown(void);
bool PortStereoLink_Enabled(void);
/* "OFF", "WAIT", "NO WI-FI" or the console's address; any thread. */
void PortStereoLink_Label(char* out, size_t size);

/* Sends Link to a room (x, y in it, -1 for its middle): 200, 404 for no such
 * room, 409 when the game is not where a warp can start. Game thread. */
int PortStereoLink_Goto(int area, int room, int x, int y, int layer);
/* A room's size in pixels; false when there is no such room. */
bool PortStereoLink_RoomSize(int area, int room, int* width, int* height);

/* Whether the game must not write its save (test mode). Any thread. */
bool PortStereoLink_SavesBlocked(void);

/* The outline of the cells the PC editor points at, as one-pixel GBA screen
 * rectangles {x, y, w, h}; returns how many. Main thread. */
int PortStereoLink_Highlight(float (*rects)[4], int max);

/* Platform: a copy of both eyes' game pictures, RGBA5551, asked for and
 * picked up a few frames later; false until it is there (or without the
 * PICA200 renderer). */
void PortStereoLink_FrameRequest(void);
bool PortStereoLink_FrameReady(const uint16_t** left, const uint16_t** right, unsigned* stride, unsigned* x0,
                               unsigned* y0);

/* Platform: the bottom screen's painted image, 320x240 ABGR words. */
bool PortStereoLink_BottomImage(const uint32_t** pixels, unsigned* pitch);

/* Platform: start sdmc:/3ds/<name> next (200; 404 no such file; 409 not run
 * from the Homebrew Launcher), and leave the game. */
int PortStereoLink_Relaunch(const char* name);
void PortStereoLink_Quit(void);

/* Platform: bring the network up (returns the address, host order) or down. */
bool PortStereoLink_NetUp(uint32_t* address);
void PortStereoLink_NetDown(void);

#endif /* PORT_STEREO_LINK_H */
