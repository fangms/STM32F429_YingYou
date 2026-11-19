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
typedef struct {
  uint16_t before_isr, before_esr, before_count, before_ack;
  uint16_t isr, esr, count, ack, final_isr;
  uint8_t command, value, bytes_read;
  bool valid;
} SSD2828_ReadReport;
/* Boot diagnostic only, VEN=0. Leaves read mode selected, even on failure.
   Caller must hardware-reset/reinitialize bridge and panel before video. */
SSD2828_Status SSD2828_VideoReadDCS(SSD2828_Video *video, uint8_t command,
                                  SSD2828_ReadReport *report);
/* Same edge is read-only. A different edge is allowed only before VideoEnable;
   active video returns RESTART_REQUIRED without pausing or changing registers. */
SSD2828_Status SSD2828_VideoSetSamplingEdge(SSD2828_Video *video, bool falling);
/* Rev1.3 8.1.56: internal colors; keep external scan timing unchanged. */
SSD2828_Status SSD2828_VideoSetBIST(SSD2828_Video *video, bool enable);
#endif
