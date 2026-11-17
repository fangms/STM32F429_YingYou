#ifndef SSD2828_VIDEO_H
#define SSD2828_VIDEO_H
#include "ssd2828_bus.h"

typedef struct {
  SSD2828_Bus *bus;
  void (*delay_ms)(uint32_t ms);
  const char *phase;
  uint16_t last_isr;
  uint8_t last_command;
  bool ready;
} SSD2828_Video;

/* Fixed candidate for this board: 24MHz crystal, 400x960 RGB565, 2 lanes. */
SSD2828_Status SSD2828_VideoConfigure(SSD2828_Video *video);
SSD2828_Status SSD2828_VideoDCS(SSD2828_Video *video, const uint8_t *bytes, uint16_t length);
SSD2828_Status SSD2828_VideoEnable(SSD2828_Video *video);
/* Preserve all bits except VICR6.PCLK_P; pause VEN around the change. */
SSD2828_Status SSD2828_VideoSetSamplingEdge(SSD2828_Video *video, bool falling);
#endif
