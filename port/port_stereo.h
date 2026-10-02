/**
 * @file port_stereo.h
 * @brief Stereoscopic 3D: a depth of its own for chosen sprites.
 *
 * A renderer that draws the layers apart for two eyes places every sprite by
 * its GBA priority. That is wrong wherever the art means something else --
 * the title logo, its subtitle and PRESS START share two priorities but stand
 * at four depths. Game code can name the depth instead; renderers that draw
 * one flat image ignore it.
 *
 * Depth is in the renderer's units, 0 on the screen plane and larger deeper:
 * backgrounds of priority 1, 2, 3 sit at 3, 6, 9.
 */
#ifndef PORT_STEREO_H
#define PORT_STEREO_H

#include "entity.h"

/* A depth as stored per OAM entry; 0 means "by priority". */
#define PORT_STEREO_DEPTH(units) ((u8)((units) + 1))

/* Depth for the sprites DrawDirect emits; set around the call, then back to 0. */
extern u8 gPortStereoDirectDepth;
/* Depth for an entity's sprites, or NULL to place every entity by priority. */
extern u8 (*gPortStereoEntityDepth)(const Entity* entity);

#endif /* PORT_STEREO_H */
