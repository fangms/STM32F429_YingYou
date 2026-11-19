#include "ssd2828_video.h"
#include "display_config.h"
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
     HS/VS active low; cold-start PCLK edge from display_config.h.
     Verify actual RGB setup/hold on the board. */
  static const struct { uint8_t reg; uint16_t value; } settings[] = {
    {0xB9u, 0x0000u}, {0xB7u, CFGR_COMMAND},
    {0xB1u, 0x080Au}, {0xB2u, 0x201Eu}, {0xB3u, 0x0C14u},
    {0xB4u, 400u}, {0xB5u, 960u}, {0xB6u, H863_DISPLAY_VICR6},
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

static SSD2828_Status ReadResponseState(SSD2828_Video *v, uint16_t *isr,
                                       uint16_t *esr, uint16_t *count, uint16_t *ack)
{
  SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xC6u, isr);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, 0xC7u, esr);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, 0xC2u, count);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, 0xC3u, ack);
  return status;
}

SSD2828_Status SSD2828_VideoReadDCS(SSD2828_Video *v, uint8_t command,
                                  SSD2828_ReadReport *r)
{
  if (r == NULL) return SSD2828_INVALID_ARGUMENT;
  *r = (SSD2828_ReadReport){.command = command};
  if (v == NULL || !v->ready || v->bus == NULL || v->delay_ms == NULL)
    return SSD2828_INVALID_ARGUMENT;
  uint16_t config = 0u;
  v->phase = "read-command-state";
  SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xB7u, &config);
  if (status != SSD2828_OK) return status;
  /* Never interrupt video, nor accept an unknown bridge configuration. */
  if (config != CFGR_COMMAND && config != 0x02C2u) return SSD2828_RESTART_REQUIRED;
  v->last_command = command;
  v->phase = "read-before-state";
  status = ReadResponseState(v, &r->before_isr, &r->before_esr,
                            &r->before_count, &r->before_ack);
  if (status != SSD2828_OK) return status;
  /* Rev1.3 p43/147: COP finishes the current packet, clears queued buffers
     and returns to initial state. Only valid before VEN. No abrupt C0.RST. */
  v->phase = "read-cancel-operation";
  status = SSD2828_WriteRegister(v->bus, 0xC0u, 1u);
  if (status != SSD2828_OK) return status;
  bool cancelled = false;
  for (unsigned poll = 0u; poll < 100u; ++poll) {
    uint16_t control = 0u;
    status = SSD2828_ReadRegister(v->bus, 0xC0u, &control);
    if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, 0xC6u, &v->last_isr);
    if (status != SSD2828_OK) return status;
    if ((control & 1u) == 0u && (v->last_isr & 2u) != 0u) { cancelled = true; break; }
    v->delay_ms(1u);
  }
  if (!cancelled) return SSD2828_WAIT_TIMEOUT;
  v->phase = "read-configure";
  /* EOT/DCS/CKE/REN, LP, VEN=0; ECD=0 enables RX ECC/CRC checks.
     Preserve Reserved C7 bit4 evidence: never write it as an error flag. */
  status = CheckedWrite(v, 0xB7u, 0x02C2u);
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xC6u, 0x001Cu);
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xC7u, 0x0781u);
  if (status == SSD2828_OK) status = CheckedWrite(v, 0xC1u, 1u);
  if (status == SSD2828_OK) status = Wait(v, 0x4000u);
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xBDu, 0u);
  if (status == SSD2828_OK) status = SSD2828_WriteRegister(v->bus, 0xBCu, 1u);
  if (status == SSD2828_OK) status = SSD2828_WritePacket(v->bus, &command, 1u);
  if (status != SSD2828_OK) return status;
  v->phase = "read-response-wait";
  status = Wait(v, 1u); /* Real RDR, not buffer-drained or reset BTAR=1. */
  SSD2828_Status capture = ReadResponseState(v, &r->isr, &r->esr, &r->count, &r->ack);
  if (capture != SSD2828_OK) return capture;
  if (status != SSD2828_OK) return status;
  v->phase = "read-response-validate";
  /* Reject even corrected RX ECC for diagnostic validity. CONT is RO bit2;
     C7 bit4 is Reserved, not a known overflow indication. */
  if ((r->isr & 1u) == 0u || r->count != 1u || r->ack != 0u ||
      (r->esr & 0x0785u) != 0u) return SSD2828_RESPONSE_INVALID;
  v->phase = "read-response-fifo";
  status = SSD2828_ReadReturnByte(v->bus, &r->value);
  if (status != SSD2828_OK) return status;
  r->bytes_read = 1u;
  v->phase = "read-bus-return";
  status = Wait(v, 4u); /* Bus authority must return from the panel. */
  r->final_isr = v->last_isr;
  if (status != SSD2828_OK) return status;
  if ((r->final_isr & 1u) != 0u) return SSD2828_RESPONSE_INVALID;
  r->valid = true;
  v->phase = "read-response-valid";
  return SSD2828_OK;
}

SSD2828_Status SSD2828_VideoSetSamplingEdge(SSD2828_Video *v, bool falling)
{
  if (v == NULL || !v->ready || v->bus == NULL || v->delay_ms == NULL)
    return SSD2828_INVALID_ARGUMENT;
  uint16_t config = 0u, interface = 0u;
  v->phase = "edge-read-config";
  SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xB7u, &config);
  if (status != SSD2828_OK) return status;
  if (config != CFGR_VIDEO && config != CFGR_COMMAND) return SSD2828_REGISTER_MISMATCH;
  v->phase = "edge-read-vicr6";
  status = SSD2828_ReadRegister(v->bus, 0xB6u, &interface);
  if (status != SSD2828_OK) return status;
  if ((interface & 0xDFFFu) != 0x0008u) return SSD2828_REGISTER_MISMATCH;
  uint16_t desired = (uint16_t)((interface & 0xDFFFu) | (falling ? 0x2000u : 0u));
  if (interface == desired) {
    v->phase = "edge-unchanged";
    return SSD2828_OK;
  }
  /* VEN=0 is not a bridge/panel reset. Never interrupt an active stream to
     hot-switch sampling, or assume restoring B6 restores pixel alignment.
     Select the alternate edge during full startup before enabling video. */
  if (config == CFGR_VIDEO) {
    v->phase = "edge-restart-required";
    return SSD2828_RESTART_REQUIRED;
  }
  v->phase = "edge-write-vicr6";
  status = CheckedWrite(v, 0xB6u, desired);
  if (status == SSD2828_OK) v->phase = "edge-ready";
  return status;
}

static SSD2828_Status WriteBISTControl(SSD2828_Video *v, uint16_t value)
{
  uint16_t actual = 0u;
  SSD2828_Status status = SSD2828_WriteRegister(v->bus, 0xEEu, value);
  if (status == SSD2828_OK) status = SSD2828_ReadRegister(v->bus, 0xEEu, &actual);
  /* Bits 15:13 are read-only status, bit13 clears on read. Never replay them.
     Bits 11/8 start calibrations; they must stay zero during this test. */
  if (status == SSD2828_OK && (actual & 0x1FFFu) != value)
    return SSD2828_REGISTER_MISMATCH;
  return status;
}

SSD2828_Status SSD2828_VideoSetBIST(SSD2828_Video *v, bool enable)
{
  if (v == NULL || !v->ready || v->bus == NULL || v->delay_ms == NULL)
    return SSD2828_INVALID_ARGUMENT;
  uint16_t config = 0u, control = 0u;
  v->phase = "bist-read-config";
  SSD2828_Status status = SSD2828_ReadRegister(v->bus, 0xB7u, &config);
  if (status != SSD2828_OK) return status;
  if (config != CFGR_VIDEO) return SSD2828_REGISTER_MISMATCH;
  v->phase = "bist-read-control";
  status = SSD2828_ReadRegister(v->bus, 0xEEu, &control);
  if (status != SSD2828_OK) return status;
  if ((control & 0x0900u) != 0u ||
      (control & 0x0600u) != (enable ? 0u : 0x0600u))
    return SSD2828_REGISTER_MISMATCH;
  /* Preserve XORC_SEL/TRIM_CMD; do not write RO flags or calibration starts. */
  uint16_t base = control & 0x10FFu;
  v->phase = enable ? "bist-enable" : "bist-stop";
  status = WriteBISTControl(v, (uint16_t)(base | 0x0200u));
  if (status != SSD2828_OK) return status;
  if (enable) {
    v->phase = "bist-start";
    status = WriteBISTControl(v, (uint16_t)(base | 0x0600u));
  } else {
    /* Datasheet: stop at a frame boundary, wait >=1 frame before disable.
       Keep LTDC running: 450*1004/27.1MHz = 16.67ms per frame. */
    v->delay_ms(20u);
    v->phase = "bist-disable";
    status = WriteBISTControl(v, base);
  }
  if (status == SSD2828_OK) v->phase = enable ? "bist-running" : "bist-disabled";
  return status;
}
