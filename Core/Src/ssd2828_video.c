#include "ssd2828_video.h"
#include <stddef.h>

#define CFGR_COMMAND 0x0342u /* EOT, ECD, DCS, CKE; LP data, crystal, VEN=0. */
#define CFGR_VIDEO   0x034Bu /* HS and VEN added, continuous clock. */

static SSD2828_Status CheckedWrite(SSD2828_Video *v, uint8_t reg, uint16_t expected)
{
  uint16_t actual = 0u;
  SSD2828_Status status = SSD2828_WriteRegister(v->bus, reg, expected);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, reg, &actual);
  if (status == SSD2828_OK && actual != expected) return SSD2828_REGISTER_MISMATCH;
  return status;
}

static SSD2828_Status Wait(SSD2828_Video *v, uint16_t mask)
{
  /* Counted polls still terminate if the time base stops advancing. */
  for (unsigned poll = 0u; poll < 100u; ++poll) {
    SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xC6u, &v->last_isr);
    if (status != SSD2828_OK) return status;
    if ((v->last_isr & mask) == mask) return SSD2828_OK;
    v->delay_ms(1u);
  }
  return SSD2828_WAIT_TIMEOUT;
}

SSD2828_Status SSD2828_VideoConfigure(SSD2828_Video *v)
{
  if (v == NULL || v->bus == NULL || v->delay_ms == NULL) return SSD2828_INVALID_ARGUMENT;
  v->ready = false;
  v->phase = "video-registers";
  /* Rev1.3 sections 8.1.2-12, 8.1.46, 9.2 and 9.3.5.
     PLL=24MHz/3*50=400Mbps/lane; LP=400/(8*(4+1))=10MHz.
     Input peak=27.1MHz*16/2=216.8Mbps/lane. Burst provides headroom.
     HS/VS active low, latch rising (PCLK_P=0); verify actual RGB setup/hold. */
  static const struct { uint8_t reg; uint16_t value; } settings[] = {
    {0xB9u, 0x0000u}, {0xB7u, CFGR_COMMAND},
    {0xB1u, 0x080Au}, {0xB2u, 0x201Eu}, {0xB3u, 0x0C14u},
    {0xB4u, 400u}, {0xB5u, 960u}, {0xB6u, 0x0008u},
    {0xB8u, 0x0000u}, {0xDEu, 0x0001u},
    {0xBAu, 0x8332u}, {0xBBu, 0x0004u},
    {0xD6u, 0x0005u}, /* TR.CO=1 (RGB); preserve default PNB=1. */
    {0xB9u, 0x0001u}
  };
  for (unsigned i = 0u; i < sizeof settings / sizeof settings[0]; ++i) {
    SSD2828_Status status = CheckedWrite(v, settings[i].reg, settings[i].value);
    if (status != SSD2828_OK) return status;
  }
  v->phase = "pll-lock";
  SSD2828_Status status = Wait(v, 0x0080u); /* ISR.PLS, bit 7. */
  if (status == SSD2828_OK) {
    v->ready = true;
    v->phase = "command-ready";
  }
  return status;
}

SSD2828_Status SSD2828_VideoDCS(SSD2828_Video *v, const uint8_t *bytes, uint16_t length)
{
  if (v == NULL || !v->ready || v->delay_ms == NULL || bytes == NULL ||
      length == 0u || length > 1023u) return SSD2828_INVALID_ARGUMENT;
  v->last_command = bytes[0];
  v->phase = "dcs-buffer-available";
  SSD2828_Status status = Wait(v, 0x4000u); /* CBA: at least one packet available. */
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xBDu, 0u);
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xBCu, length);
  v->phase = status == SSD2828_OK ? "dcs-fifo" : v->phase;
  if (status == SSD2828_OK) status = SSD2828_WritePacket(v->bus, bytes, length);
  if (status != SSD2828_OK) return status;
  v->phase = "dcs-buffer-drain";
  /* CBE confirms bridge transmission only, not panel acceptance (no BTA). */
  return Wait(v, 0x8000u);
}

SSD2828_Status SSD2828_VideoEnable(SSD2828_Video *v)
{
  if (v == NULL || !v->ready || v->delay_ms == NULL) return SSD2828_INVALID_ARGUMENT;
  v->phase = "video-enable";
  return CheckedWrite(v, 0xB7u, CFGR_VIDEO);
}

SSD2828_Status SSD2828_VideoSetSamplingEdge(SSD2828_Video *v, bool falling)
{
  if (v == NULL || !v->ready || v->bus == NULL || v->delay_ms == NULL)
    return SSD2828_INVALID_ARGUMENT;
  uint16_t config = 0u, interface = 0u;
  v->phase = "edge-read-config";
  SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xB7u, &config);
  if (status != SSD2828_OK) return status;
  if (config != CFGR_VIDEO) return SSD2828_REGISTER_MISMATCH;
  v->phase = "edge-video-pause";
  /* Rev1.3 table 8-9: VEN is bit 3. Keep HS (bit 0)/CKE unchanged. */
  status = CheckedWrite(v, 0xB7u, (uint16_t)(config & ~0x0008u));
  if (status != SSD2828_OK) return status;
  v->delay_ms(20u); /* More than one 450x1004 / 27.1MHz frame. */
  v->phase = "edge-read-vicr6";
  status = SSD2828_ReadRegister(v->bus, 0xB6u, &interface);
  if (status != SSD2828_OK) return status;
  if ((interface & 0xDFFFu) != 0x0008u) return SSD2828_REGISTER_MISMATCH;
  v->phase = "edge-write-vicr6";
  interface = (uint16_t)((interface & 0xDFFFu) | (falling ? 0x2000u : 0u));
  status = CheckedWrite(v, 0xB6u, interface);
  if (status != SSD2828_OK) return status;
  v->phase = "edge-video-resume";
  status = CheckedWrite(v, 0xB7u, config);
  if (status == SSD2828_OK) v->phase = "edge-ready";
  return status;
}
