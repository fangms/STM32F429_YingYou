#ifndef BOARD_SSD2828_H
#define BOARD_SSD2828_H

#include "ssd2828_bus.h"

typedef struct {
  const char *phase;
  uint16_t last_isr;
  uint8_t last_command;
  bool commands_sent, video_enabled;
} BoardSSD2828_DisplayReport;

SSD2828_Status BoardSSD2828_Probe(SSD2828_ProbeReport *report);
/* Call once after probe PASS and LTDC starts with valid pixels or a disabled layer/background.
   Boot-only; on failure panel/bridge power and backlight are shut down. */
SSD2828_Status BoardSSD2828_StartPanel(bool rgb_scan_ready, BoardSSD2828_DisplayReport *report);

#define BOARD_SSD2828_SNAPSHOT_COUNT 16u
typedef struct {
  uint8_t registers[BOARD_SSD2828_SNAPSHOT_COUNT];
  uint16_t values[BOARD_SSD2828_SNAPSHOT_COUNT];
  unsigned count;
} BoardSSD2828_Snapshot;
SSD2828_Status BoardSSD2828_ReadSnapshot(BoardSSD2828_Snapshot *snapshot);
SSD2828_Status BoardSSD2828_SetSamplingEdge(bool falling, BoardSSD2828_DisplayReport *report);
void BoardSSD2828_Shutdown(void);
#endif
