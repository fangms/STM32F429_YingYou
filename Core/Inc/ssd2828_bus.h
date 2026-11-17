#ifndef SSD2828_BUS_H
#define SSD2828_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { SSD2828_SIGNAL_CS, SSD2828_SIGNAL_CLOCK, SSD2828_SIGNAL_MOSI } SSD2828_Signal;
typedef struct {
  void *context;
  void (*write)(void *context, SSD2828_Signal signal, bool high);
  bool (*read_miso)(void *context);
  bool (*delay_us)(void *context, uint32_t microseconds);
} SSD2828_IO;

typedef struct { SSD2828_IO io; bool initialized; } SSD2828_Bus;
typedef enum {
  SSD2828_OK = 0,
  SSD2828_INVALID_ARGUMENT,
  SSD2828_TIMING_FAILED,
  SSD2828_ID_MISMATCH,
  SSD2828_REGISTER_MISMATCH,
  SSD2828_RESTORE_FAILED,
  SSD2828_WAIT_TIMEOUT
} SSD2828_Status;

typedef struct {
  const char *phase;
  uint16_t id[3];
  uint8_t id_count;
  uint16_t original, expected, actual, restored_value;
  bool register_test_started, register_restored;
} SSD2828_ProbeReport;

/* SSD2828 Rev1.3 section 10.2: write=DC+8 bits, read data=8 bits, LSB byte first.
   Boot-only, single caller. IO must remain valid and must not call the bus recursively. */
SSD2828_Status SSD2828_BusInit(SSD2828_Bus *bus, const SSD2828_IO *io);
SSD2828_Status SSD2828_WriteRegister(SSD2828_Bus *bus, uint8_t reg, uint16_t value);
SSD2828_Status SSD2828_ReadRegister(SSD2828_Bus *bus, uint8_t reg, uint16_t *value);
/* 8bit interface FIFO: exact byte count, including the DCS command; no padding. */
SSD2828_Status SSD2828_WritePacket(SSD2828_Bus *bus, const uint8_t *bytes, uint16_t length);
/* Run with bridge freshly reset and video disabled; restores VICR4 after testing. */
SSD2828_Status SSD2828_Probe(SSD2828_Bus *bus, SSD2828_ProbeReport *report);

#endif
