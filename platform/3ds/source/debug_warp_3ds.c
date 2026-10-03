/* Developer aids. `debug_warp=area,room,x,y,layer` in tmc3ds.ini sends Link to
 * that room once he can move, so a test run (an emulator driven by a script,
 * say) can look at any room without playing to it. It takes the engine's own
 * exit path -- the one area exits and the PC port's debug menu use -- so the
 * room loads as it would in play. `debug_stereo_editor=1` (2: with its help) opens the 3D
 * editor once he has been able to move for five seconds with no key held (an
 * emulator cannot tap the bottom screen, and a script mashing through the
 * intro would close it). There is no UI for either and neither is written back to the ini. */
#include "global.h"
#include "main.h"
#include "player.h"
#include "room.h"
#include "save.h"
#include "transitions.h"
#include "common.h"

#include "port_stereo_editor.h"

extern bool Port_Config_3DSDebugWarp(unsigned out[5]);
extern int Port_Config_3DSDebugStereoEditor(void);

static void DebugStereoEditorTick(void) {
    static bool done;
    static unsigned settled;
    if (done || !Port_Config_3DSDebugStereoEditor()) {
        return;
    }
    if (gMain.task != TASK_GAME || gPlayerState.controlMode != CONTROL_ENABLED || gInput.heldKeys != 0) {
        settled = 0;
        return;
    }
    if (++settled >= 300) {
        done = true;
        PortStereoEditor_Open();
        if (Port_Config_3DSDebugStereoEditor() == 2) {
            PortStereoEditor_ShowHelp();
        }
    }
}

void Port_3DS_DebugWarpTick(void) {
    DebugStereoEditorTick();
    static bool done;
    static unsigned settled;
    unsigned warp[5];
    if (done || !Port_Config_3DSDebugWarp(warp)) {
        return;
    }
    /* Wait until the player has had control for a second: the opening
     * cutscenes own the camera and the room until then. */
    if (gMain.task != TASK_GAME || gPlayerState.controlMode != CONTROL_ENABLED || gSave.stats.health == 0) {
        settled = 0;
        return;
    }
    if (++settled < 60) {
        return;
    }
    done = true;

    Transition t = { 0 };
    t.warp_type = WARP_TYPE_AREA;
    t.area = (u8)warp[0];
    t.room = (u8)warp[1];
    t.endX = (u16)warp[2];
    t.endY = (u16)warp[3];
    t.layer = (u8)warp[4];
    gRoomTransition.stairs_idx = 0; /* or StairsAreValid() cancels the exit */
    DoExitTransition(&t);
}
