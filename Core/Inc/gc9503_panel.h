#ifndef GC9503_PANEL_H
#define GC9503_PANEL_H
#include "ssd2828_video.h"
/* Factory BOE3.99 400x960 table + RGB565 COLMOD=50. No GRAM assumed. */
SSD2828_Status GC9503_Initialize(SSD2828_Video *video);
#endif
