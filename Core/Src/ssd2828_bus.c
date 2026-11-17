#include "ssd2828_bus.h"
#include <stddef.h>

#define REG_DIR 0xB0u
#define REG_VICR4 0xB4u
#define REG_LRR 0xD4u
#define READ_COMMAND 0xFAu
#define EXPECTED_ID 0x2828u

static bool Valid(const SSD2828_Bus *bus)
{
  return bus != NULL && bus->initialized;
}

static void Write(SSD2828_Bus *bus, SSD2828_Signal signal, bool high)
{
  bus->io.write(bus->io.context, signal, high);
}

static bool Delay(SSD2828_Bus *bus, uint32_t microseconds)
{
  return bus->io.delay_us(bus->io.context, microseconds);
}

static void Release(SSD2828_Bus *bus)
{
  Write(bus, SSD2828_SIGNAL_CLOCK, false);
  Write(bus, SSD2828_SIGNAL_CS, true);
  Write(bus, SSD2828_SIGNAL_MOSI, false);
}

SSD2828_Status SSD2828_BusInit(SSD2828_Bus *bus, const SSD2828_IO *io)
{
  if (bus == NULL) return SSD2828_INVALID_ARGUMENT;
  *bus = (SSD2828_Bus){0};
  if (io == NULL || io->write == NULL || io->read_miso == NULL || io->delay_us == NULL) {
    return SSD2828_INVALID_ARGUMENT;
  }
  bus->io = *io;
  Release(bus);
  if (!Delay(bus, 2u)) return SSD2828_TIMING_FAILED;
  bus->initialized = true;
  return SSD2828_OK;
}

static bool Begin(SSD2828_Bus *bus)
{
  Write(bus, SSD2828_SIGNAL_CLOCK, false);
  Write(bus, SSD2828_SIGNAL_CS, false);
  return Delay(bus, 2u);
}

static SSD2828_Status End(SSD2828_Bus *bus, bool success)
{
  if (success) success = Delay(bus, 2u);
  Release(bus);
  if (success) success = Delay(bus, 2u);
  return success ? SSD2828_OK : SSD2828_TIMING_FAILED;
}

static bool WriteBit(SSD2828_Bus *bus, bool high)
{
  Write(bus, SSD2828_SIGNAL_CLOCK, false);
  Write(bus, SSD2828_SIGNAL_MOSI, high);
  if (!Delay(bus, 1u)) return false;
  Write(bus, SSD2828_SIGNAL_CLOCK, true);
  return Delay(bus, 1u);
}

static bool WriteByte(SSD2828_Bus *bus, bool data, uint8_t value)
{
  if (!WriteBit(bus, data)) return false;
  for (uint32_t bit = 0u; bit < 8u; ++bit) {
    if (!WriteBit(bus, (value & 0x80u) != 0u)) return false;
    value = (uint8_t)(value << 1u);
  }
  return true;
}

static bool ReadByte(SSD2828_Bus *bus, uint8_t *value)
{
  uint8_t result = 0u;
  Write(bus, SSD2828_SIGNAL_MOSI, false);
  /* Read cycles have exactly 8 clocks: do not insert a DC/dummy bit. */
  for (uint32_t bit = 0u; bit < 8u; ++bit) {
    Write(bus, SSD2828_SIGNAL_CLOCK, false);
    if (!Delay(bus, 1u)) return false;
    Write(bus, SSD2828_SIGNAL_CLOCK, true);
    result = (uint8_t)((result << 1u) | (bus->io.read_miso(bus->io.context) ? 1u : 0u));
    if (!Delay(bus, 1u)) return false;
  }
  *value = result;
  return true;
}

SSD2828_Status SSD2828_WriteRegister(SSD2828_Bus *bus, uint8_t reg, uint16_t value)
{
  if (!Valid(bus) || reg < REG_DIR) return SSD2828_INVALID_ARGUMENT;
  bool success = Begin(bus) && WriteByte(bus, false, reg) &&
                 WriteByte(bus, true, (uint8_t)value) &&
                 WriteByte(bus, true, (uint8_t)(value >> 8u));
  return End(bus, success);
}

SSD2828_Status SSD2828_ReadRegister(SSD2828_Bus *bus, uint8_t reg, uint16_t *value)
{
  uint8_t low = 0u, high = 0u;
  if (!Valid(bus) || reg < REG_DIR || value == NULL) return SSD2828_INVALID_ARGUMENT;
  bool success = Begin(bus) && WriteByte(bus, false, reg) &&
                 WriteByte(bus, false, READ_COMMAND) &&
                 ReadByte(bus, &low) && ReadByte(bus, &high);
  SSD2828_Status status = End(bus, success);
  if (status == SSD2828_OK) *value = (uint16_t)(low | ((uint16_t)high << 8u));
  return status;
}

SSD2828_Status SSD2828_WritePacket(SSD2828_Bus *bus, const uint8_t *bytes, uint16_t length)
{
  if (!Valid(bus) || bytes == NULL || length == 0u || length > 1023u) {
    return SSD2828_INVALID_ARGUMENT;
  }
  bool success = Begin(bus) && WriteByte(bus, false, 0xBFu);
  for (uint16_t i = 0u; success && i < length; ++i) {
    success = WriteByte(bus, true, bytes[i]);
  }
  return End(bus, success);
}

SSD2828_Status SSD2828_Probe(SSD2828_Bus *bus, SSD2828_ProbeReport *report)
{
  SSD2828_Status status, test_status;
  if (report == NULL) return SSD2828_INVALID_ARGUMENT;
  *report = (SSD2828_ProbeReport){0};
  report->phase = "configure-read-command";
  status = SSD2828_WriteRegister(bus, REG_LRR, READ_COMMAND);
  if (status != SSD2828_OK) return status;

  report->phase = "read-id-3-times";
  for (uint32_t i = 0u; i < 3u; ++i) {
    status = SSD2828_ReadRegister(bus, REG_DIR, &report->id[i]);
    if (status != SSD2828_OK) return status;
    ++report->id_count;
  }
  for (uint32_t i = 0u; i < 3u; ++i) {
    if (report->id[i] != EXPECTED_ID) return SSD2828_ID_MISMATCH;
  }

  report->phase = "save-vicr4";
  status = SSD2828_ReadRegister(bus, REG_VICR4, &report->original);
  if (status != SSD2828_OK) return status;
  report->expected = 0x0190u; /* Asymmetric bytes also expose byte-order mistakes. */
  report->phase = "test-vicr4";
  report->register_test_started = true;
  test_status = SSD2828_WriteRegister(bus, REG_VICR4, report->expected);
  if (test_status == SSD2828_OK) {
    test_status = SSD2828_ReadRegister(bus, REG_VICR4, &report->actual);
    if (test_status == SSD2828_OK && report->actual != report->expected) {
      test_status = SSD2828_REGISTER_MISMATCH;
    }
  }

  /* A failed/partial write might have changed the register: always try restoring. */
  report->phase = "restore-vicr4";
  status = SSD2828_WriteRegister(bus, REG_VICR4, report->original);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(bus, REG_VICR4, &report->restored_value);
  if (status != SSD2828_OK || report->restored_value != report->original) {
    return SSD2828_RESTORE_FAILED;
  }
  report->register_restored = true;
  report->phase = test_status == SSD2828_OK ? "probe-pass" : "test-vicr4";
  return test_status;
}
