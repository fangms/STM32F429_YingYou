#ifndef DISPLAY_SURFACE_H
#define DISPLAY_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Physical scan stays 400x960/800-byte pitch for every drawing direction. */
#define DISPLAY_NATIVE_WIDTH 400u
#define DISPLAY_NATIVE_HEIGHT 960u
#define DISPLAY_NATIVE_FRAME_BYTES (DISPLAY_NATIVE_WIDTH * DISPLAY_NATIVE_HEIGHT * 2u)
typedef enum {
  DISPLAY_ROTATION_0 = 0,
  DISPLAY_ROTATION_CW90 = 90,
  DISPLAY_ROTATION_CCW90 = 270
} DisplayRotation;

typedef struct {
  volatile uint16_t *pixels;
  unsigned width, height;
  DisplayRotation rotation;
} DisplaySurface;

static inline bool DisplaySurface_Init(DisplaySurface *s, volatile uint16_t *pixels,
                                       size_t capacity, DisplayRotation rotation)
{
  if (s == NULL || pixels == NULL || ((uintptr_t)pixels & 1u) != 0u ||
      capacity < DISPLAY_NATIVE_FRAME_BYTES ||
      (rotation != DISPLAY_ROTATION_0 && rotation != DISPLAY_ROTATION_CW90 &&
       rotation != DISPLAY_ROTATION_CCW90)) return false;
  *s = (DisplaySurface){.pixels=pixels, .rotation=rotation,
      .width=rotation == DISPLAY_ROTATION_0 ? DISPLAY_NATIVE_WIDTH : DISPLAY_NATIVE_HEIGHT,
      .height=rotation == DISPLAY_ROTATION_0 ? DISPLAY_NATIVE_HEIGHT : DISPLAY_NATIVE_WIDTH};
  return true;
}

/* Internal drawing primitive: caller supplies x<width, y<height.
   CW: (x,y)->(399-y,x). CCW: (x,y)->(y,959-x). No scaling or extra buffer. */
static inline size_t DisplaySurface_Index(const DisplaySurface *s, unsigned x, unsigned y)
{
  if (s->rotation == DISPLAY_ROTATION_CW90)
    return (size_t)x * DISPLAY_NATIVE_WIDTH + DISPLAY_NATIVE_WIDTH - 1u - y;
  if (s->rotation == DISPLAY_ROTATION_CCW90)
    return (size_t)(DISPLAY_NATIVE_HEIGHT - 1u - x) * DISPLAY_NATIVE_WIDTH + y;
  return (size_t)y * DISPLAY_NATIVE_WIDTH + x;
}

static inline void DisplaySurface_Pixel(const DisplaySurface *s, unsigned x, unsigned y, uint16_t color)
{
  s->pixels[DisplaySurface_Index(s, x, y)] = color;
}
#endif
