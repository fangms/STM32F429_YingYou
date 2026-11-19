#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

/* Cold-start profile. Select before VideoEnable; never hot-switch to compare.
   SSD2828 Rev1.3 p31: bit13=1 latches RGB on the falling PCLK edge.
   This is a diagnostic candidate, not proof of panel signal integrity. */
#define H863_DISPLAY_SAMPLING_FALLING 1

/* UI coordinates only; physical LTDC/bridge/panel timing remains 400x960. */
#ifndef H863_DISPLAY_ROTATION
#define H863_DISPLAY_ROTATION 90u
#endif
#if H863_DISPLAY_ROTATION == 90u
#define H863_DISPLAY_ORIENTATION_NAME "LANDSCAPE CW90 960x400"
#elif H863_DISPLAY_ROTATION == 270u
#define H863_DISPLAY_ORIENTATION_NAME "LANDSCAPE CCW90 960x400"
#else
#error "Select 90 (clockwise) or 270 (counterclockwise) for the landscape UI"
#endif

#if H863_DISPLAY_SAMPLING_FALLING
#define H863_DISPLAY_VICR6 0x2008u
#define H863_DISPLAY_EDGE_NAME "FALLING"
#define H863_DISPLAY_FIXED_LABEL "B/FALLING"
#define H863_DISPLAY_CARD_PATTERN "fixed-falling-card"
#else
#define H863_DISPLAY_VICR6 0x0008u
#define H863_DISPLAY_EDGE_NAME "RISING"
#define H863_DISPLAY_FIXED_LABEL "A/RISING"
#define H863_DISPLAY_CARD_PATTERN "fixed-rising-card"
#endif

#endif
