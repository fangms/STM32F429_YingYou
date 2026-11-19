#include "ssd2828_bus.h"
#include "gc9503_panel.h"
#include "board_screen.h"
#include "board_display_diag.h"
#include "display_config.h"
#include "board_ssd2828.h"
#include "display_test.h"
#include "main.h"
#include "debug_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "factory_expected.h"

typedef struct {
  uint16_t registers[256], ids[3], returned;
  bool cs, clock, mosi, bits[9216], timer_running;
  unsigned edges, writes, reads, aborts, id_reads, vicr4_reads;
  unsigned delay_calls, fail_delay, corrupt_test, corrupt_restore, gpio_writes;
  unsigned power18_on, power12_on, reset_released, sequence;
  uint32_t tick, hclk, power28_tick, last_packet_tick;
  uint8_t packets[128][1023];
  uint16_t lengths[128];
  unsigned packet_count, gpio_inits, stop_after_packet, corrupt_reg;
  bool never_lock, block_cba, block_cbe, reject_address;
  unsigned corrupt_reg_read, reg_reads[256], reg_writes[256], video_pauses;
  unsigned b6_video_on_writes, b6_backlight_on_writes;
  uint16_t bist_writes[16];
  uint32_t bist_write_ticks[16];
  unsigned bist_write_count;
  unsigned read_requests, fifo_reads, fifo26_reads, hardware_resets;
  unsigned rx_delay, rx_waits;
  uint16_t rx_count, rx_ack, rx_errors;
  bool block_rdr, block_btar, stuck_cancel, read_pending, bad_restore_id;
  uint8_t rx_value;
  LTDC_HandleTypeDef *scan_ltdc;
  uint16_t *scan_frame;
  uint32_t active_hash, active_address, active_background, active_rgb;
  unsigned active_frame_violations, commits;
  bool active_layer, stuck_reload;
} Model;

static Model model;
GPIO_TypeDef test_gpio[9];
CoreDebug_Type test_coredebug;
FMC_SDRAM_TypeDef test_fmc;
uint32_t test_ipsr, test_primask;
static DWT_Type test_dwt;
static char uart_text[262144];
static uint32_t FrameHash(const uint16_t *pixels);
static size_t uart_length;

static unsigned Decode(unsigned offset, unsigned count)
{
  unsigned result = 0u;
  for (unsigned i = 0u; i < count; ++i) result = (result << 1u) | model.bits[offset + i];
  return result;
}

static void WireWrite(void *context, SSD2828_Signal signal, bool high)
{
  assert(context == &model);
  if (signal == SSD2828_SIGNAL_CS) {
    if (model.cs && !high) {
      model.edges = 0u;
      memset(model.bits, 0, sizeof model.bits);
    } else if (!model.cs && high) {
      if (model.edges >= 18u && Decode(1u, 8u) == 0xBFu && model.bits[9]) {
        unsigned length = model.edges / 9u - 1u;
        if (model.edges % 9u == 0u && length == model.registers[0xBC]) {
          assert(model.packet_count < 128u && length <= 1023u);
          for (unsigned i = 0u; i < length; ++i) {
            assert(model.bits[(i + 1u) * 9u]);
            model.packets[model.packet_count][i] = (uint8_t)Decode((i + 1u) * 9u + 1u, 8u);
          }
          model.lengths[model.packet_count++] = (uint16_t)length;
          model.last_packet_tick = model.tick;
          if ((model.registers[0xB7] & 0x80u) != 0u) {
            assert(length == 1u && model.registers[0xB7] == 0x02C2u);
            assert(model.registers[0xC1] == 1u && model.registers[0xBD] == 0u);
            assert((model.registers[0xB7] & 8u) == 0u);
            ++model.read_requests;
            uint8_t command = model.packets[model.packet_count - 1u][0];
            model.rx_value = command == 0x0Cu ? 0x50u : command == 0x0Au ? 0x1Cu : 0u;
            model.registers[0xC2] = model.rx_count;
            model.registers[0xC3] = model.rx_ack;
            model.registers[0xC7] |= model.rx_errors;
            model.registers[0xC6] &= ~5u;
            model.read_pending = true; model.rx_waits = 0u;
          }
        } else ++model.aborts;
      } else if (model.edges == 27u && model.bits[9]) {
        assert(Decode(0u, 1u) == 0u);
        assert(Decode(9u, 1u) == 1u && Decode(18u, 1u) == 1u);
        unsigned reg = Decode(1u, 8u);
        if (reg == 0xB7u && Decode(10u, 8u) == 0x43u) {
          /* Datasheet table 8-9: pause must be 0343 (VEN bit3=0, HS bit0=1). */
          assert(Decode(19u, 8u) == 0x03u);
        }
        if (reg == 0xB6u) {
          if ((model.registers[0xB7] & 0x0008u) != 0u) ++model.b6_video_on_writes;
          if ((PMU_EN4_GPIO_Port->ODR & PMU_EN4_Pin) != 0u) ++model.b6_backlight_on_writes;
        }
        uint16_t written = (uint16_t)(Decode(10u, 8u) | (Decode(19u, 8u) << 8u));
        ++model.reg_writes[reg];
        if (reg == 0xB7u && (model.registers[reg] & 8u) != 0u && (written & 8u) == 0u)
          ++model.video_pauses;
        if (reg == 0xEEu) {
          assert(model.bist_write_count < 16u);
          model.bist_writes[model.bist_write_count] = written;
          model.bist_write_ticks[model.bist_write_count++] = model.tick;
          model.registers[reg] = (uint16_t)((model.registers[reg] & 0xE000u) | (written & 0x1FFFu));
        } else if (reg == 0xC6u) {
          model.registers[reg] &= ~(written & 0x7Cu); /* Only RW1C status bits. */
        } else if (reg == 0xC7u) {
          model.registers[reg] &= ~(written & 0x0791u); /* Includes Reserved bit4. */
        } else if (reg == 0xC0u && written == 1u && !model.stuck_cancel) {
          assert((model.registers[0xB7] & 8u) == 0u);
          model.registers[reg] = 0u; model.read_pending = false;
          model.registers[0xC2] = model.registers[0xC3] = 0u;
          model.registers[0xC6] = (model.registers[0xC6] & ~1u) | 2u;
        } else model.registers[reg] = written;
        ++model.writes;
      } else if (!model.bits[9] && Decode(10u, 8u) == 0xFAu &&
                 ((model.edges == 34u && Decode(1u, 8u) != 0xFFu) ||
                  (model.edges == 26u && Decode(1u, 8u) == 0xFFu))) {
        assert(!model.bits[0] && !model.bits[9]);
        assert(Decode(10u, 8u) == 0xFAu);
        unsigned reg = Decode(1u, 8u);
        assert((reg == 0xFFu) == (model.edges == 26u));
        if (reg == 0xFFu) {
          ++model.fifo26_reads;
          model.registers[0xC6] &= ~1u; /* Single requested byte fully consumed. */
        }
        ++model.reads;
      } else {
        ++model.aborts;
      }
    }
    model.cs = high;
  } else if (signal == SSD2828_SIGNAL_CLOCK) {
    if (!model.clock && high && !model.cs) {
      assert(model.edges < 9216u);
      model.bits[model.edges++] = model.mosi;
    }
    model.clock = high;
  } else {
    model.mosi = high;
  }
}

static bool WireRead(void *context)
{
  assert(context == &model && !model.cs && model.clock);
  assert(model.edges >= 19u && model.edges <= 34u);
  assert(Decode(10u, 8u) == 0xFAu && model.registers[0xD4] == 0xFAu);
  unsigned reg = Decode(1u, 8u), index = model.edges - 19u;
  if (index == 0u) {
    if (reg == 0xC6u && model.read_pending && ++model.rx_waits > model.rx_delay) {
      if (!model.block_rdr) model.registers[0xC6] |= 1u;
      if (!model.block_btar) model.registers[0xC6] |= 4u;
      model.read_pending = false;
    }
    model.returned = model.registers[reg];
    if (reg == 0xFFu) {
      assert((model.registers[0xC6] & 1u) != 0u);
      assert(model.edges <= 26u); ++model.fifo_reads;
      model.returned = model.rx_value;
    }
    if (reg == 0xEEu) model.registers[reg] &= ~0x2000u; /* XORC_DONE read-clear. */
    ++model.reg_reads[reg];
    if (reg == model.corrupt_reg && (model.corrupt_reg_read == 0u ||
        model.reg_reads[reg] == model.corrupt_reg_read)) model.returned ^= 1u;
    if (reg == 0xC6u) {
      if (model.never_lock) model.returned &= ~0x0080u;
      if (model.block_cba) model.returned &= ~0x4000u;
      if (model.block_cbe || (model.stop_after_packet != 0u &&
          model.packet_count >= model.stop_after_packet)) model.returned &= ~0x8000u;
    }
    if (reg == 0xB0u) {
      model.returned = model.bad_restore_id && model.read_requests != 0u
        ? 0xFFFFu : model.ids[model.id_reads % 3u];
      ++model.id_reads;
    }
    if (reg == 0xB4u) {
      ++model.vicr4_reads;
      if (model.corrupt_test && model.vicr4_reads == 2u) model.returned ^= 1u;
      if (model.corrupt_restore && model.vicr4_reads == 3u) model.returned ^= 1u;
    }
  }
  uint8_t byte = index < 8u ? (uint8_t)model.returned : (uint8_t)(model.returned >> 8u);
  return ((byte >> (7u - index % 8u)) & 1u) != 0u;
}

static bool WireDelay(void *context, uint32_t us)
{
  assert(context == &model && (us == 1u || us == 2u));
  return ++model.delay_calls != model.fail_delay;
}

static void Reset(void)
{
  memset(&model, 0, sizeof model);
  memset(test_gpio, 0, sizeof test_gpio);
  memset(&test_dwt, 0, sizeof test_dwt);
  test_ipsr = test_primask = 0u;
  model.cs = true;
  model.timer_running = true;
  model.hclk = 180000000u;
  model.registers[0xD4] = 0xFAu;
  model.registers[0xC6] = 0xC086u;
  model.rx_count = 1u;
  model.registers[0xB4] = 0x0234u;
  model.ids[0] = model.ids[1] = model.ids[2] = 0x2828u;
  test_gpio[2].ODR = SPI_NSS_Pin;
  uart_length = 0u;
  uart_text[0] = '\0';
}

static SSD2828_Bus Bus(void)
{
  SSD2828_Bus bus;
  SSD2828_IO io = {&model, WireWrite, WireRead, WireDelay};
  assert(SSD2828_BusInit(&bus, &io) == SSD2828_OK);
  return bus;
}

static void TestProtocol(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  assert(SSD2828_WriteRegister(&bus, 0xB1u, 0xA35Cu) == SSD2828_OK);
  assert(model.registers[0xB1] == 0xA35Cu && model.writes == 1u);
  uint16_t value = 0u;
  assert(SSD2828_ReadRegister(&bus, 0xB1u, &value) == SSD2828_OK);
  assert(value == 0xA35Cu && model.reads == 1u && model.cs && !model.clock);
  unsigned delays = model.delay_calls;
  assert(SSD2828_ReadRegister(&bus, 0xB1u, NULL) == SSD2828_INVALID_ARGUMENT);
  assert(SSD2828_WriteRegister(&bus, 0x20u, 0u) == SSD2828_INVALID_ARGUMENT);
  assert(model.delay_calls == delays);
  model.fail_delay = delays + 8u;
  value = 0x1234u;
  assert(SSD2828_ReadRegister(&bus, 0xB1u, &value) == SSD2828_TIMING_FAILED);
  assert(value == 0x1234u && model.cs && !model.clock && model.aborts == 1u);
  assert(SSD2828_ReadRegister(&bus, 0xB1u, &value) == SSD2828_OK && value == 0xA35Cu);
  assert(SSD2828_BusInit(&bus, NULL) == SSD2828_INVALID_ARGUMENT);
  assert(SSD2828_ReadRegister(&bus, 0xB1u, &value) == SSD2828_INVALID_ARGUMENT);
  puts("PASS wire protocol: 27-clock write, 34-clock read, byte order, timeout cleanup/recovery");
}

static void TestProbe(void)
{
  SSD2828_ProbeReport report;
  Reset();
  SSD2828_Bus bus = Bus();
  assert(SSD2828_Probe(&bus, &report) == SSD2828_OK);
  assert(report.id_count == 3u && report.register_restored);
  assert(report.original == 0x0234u && report.actual == 0x0190u);
  assert(model.registers[0xB4] == 0x0234u && report.restored_value == 0x0234u);
  for (unsigned fault = 0u; fault < 3u; ++fault) {
    Reset(); bus = Bus();
    model.ids[1] = fault == 0u ? 0u : fault == 1u ? 0xFFFFu : 0x2829u;
    assert(SSD2828_Probe(&bus, &report) == SSD2828_ID_MISMATCH);
    assert(report.id_count == 3u && !report.register_test_started && model.registers[0xB4] == 0x0234u);
  }
  Reset(); bus = Bus(); model.corrupt_test = 1u;
  assert(SSD2828_Probe(&bus, &report) == SSD2828_REGISTER_MISMATCH);
  assert(report.actual == 0x0191u && report.register_restored && model.registers[0xB4] == 0x0234u);
  Reset(); bus = Bus(); model.corrupt_restore = 1u;
  assert(SSD2828_Probe(&bus, &report) == SSD2828_RESTORE_FAILED);
  assert(!report.register_restored && strcmp(report.phase, "restore-vicr4") == 0);
  /* Inject a one-shot timing failure at every delay in a successful probe. */
  Reset(); bus = Bus(); assert(SSD2828_Probe(&bus, &report) == SSD2828_OK);
  unsigned total_delays = model.delay_calls;
  for (unsigned injection = 2u; injection <= total_delays; ++injection) {
    Reset(); bus = Bus(); model.fail_delay = injection;
    assert(SSD2828_Probe(&bus, &report) != SSD2828_OK);
    assert(model.cs && !model.clock);
    if (report.register_restored) assert(model.registers[0xB4] == 0x0234u);
    if (report.register_test_started && !report.register_restored) {
      assert(strcmp(report.phase, "restore-vicr4") == 0);
    }
  }
  puts("PASS probe: repeated IDs, asymmetric register readback/restore, corruption, all delay failures");
}

uint32_t HAL_RCC_GetHCLKFreq(void) { return model.hclk; }
uint32_t HAL_GetTick(void) { return model.tick; }
/* RM0090 16.4.2/16.7.21: disabled layer still blends DCCR, bottom up.
   This model intentionally distinguishes CA and PA*CA, unlike the V0.03 stub. */
static uint32_t BlendDefault(const LTDC_Layer_TypeDef *layer, uint32_t below)
{
  assert(layer->BFCR == 0x405u || layer->BFCR == 0x607u);
  unsigned alpha = layer->CACR & 255u;
  if (layer->BFCR == 0x607u) alpha = alpha * (layer->DCCR >> 24u) / 255u;
  uint32_t out = 0u;
  for (unsigned shift = 0u; shift <= 16u; shift += 8u) {
    unsigned fg = (layer->DCCR >> shift) & 255u, bg = (below >> shift) & 255u;
    out |= ((fg * alpha + bg * (255u - alpha)) / 255u) << shift;
  }
  return out;
}
static void PollDisplay(void)
{
  if (model.scan_ltdc == NULL) return;
  LTDC_TypeDef *r = model.scan_ltdc->Instance;
  /* ICR is write-one-to-clear; shadow changes only take effect at reload. */
  r->ISR &= ~r->ICR;
  r->ICR = 0u;
  if (model.active_layer && model.scan_frame != NULL &&
      FrameHash(model.scan_frame) != model.active_hash) ++model.active_frame_violations;
  if ((r->SRCR & LTDC_SRCR_VBR) != 0u && !model.stuck_reload) {
    model.active_layer = (r->layers[0].CR & LTDC_LxCR_LEN) != 0u;
    model.active_address = r->layers[0].CFBAR;
    model.active_background = r->BCCR;
    if (!model.active_layer && (r->layers[1].CR & LTDC_LxCR_LEN) == 0u)
      model.active_rgb = BlendDefault(&r->layers[1], BlendDefault(&r->layers[0], r->BCCR));
    if (model.active_layer && model.scan_frame != NULL) model.active_hash = FrameHash(model.scan_frame);
    r->SRCR &= ~LTDC_SRCR_VBR;
    ++model.commits;
  }
}
void HAL_Delay(uint32_t ms) { PollDisplay(); model.tick += ms; }
DWT_Type *TestDWT(void)
{
  if (model.timer_running) test_dwt.CYCCNT += 250u;
  return &test_dwt;
}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, const uint8_t *data,
                                  uint16_t size, uint32_t timeout)
{
  assert(uart != NULL && timeout == 100u && uart_length + size < sizeof uart_text);
  memcpy(uart_text + uart_length, data, size);
  uart_length += size;
  uart_text[uart_length] = '\0';
  return HAL_OK;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
  bool high = state == GPIO_PIN_SET;
  ++model.gpio_writes;
  if (port == SPI_NSS_GPIO_Port && pin == SPI_NSS_Pin) WireWrite(&model, SSD2828_SIGNAL_CS, high);
  else if (port == SPI_CLK_GPIO_Port && pin == SPI_CLK_Pin) WireWrite(&model, SSD2828_SIGNAL_CLOCK, high);
  else if (port == SPI_MOSI_GPIO_Port && pin == SPI_MOSI_Pin) WireWrite(&model, SSD2828_SIGNAL_MOSI, high);
  else {
    ++model.sequence;
    if (port == LCDBL_EN_GPIO_Port && pin == LCDBL_EN_Pin) assert(high);
    assert(!(port == SPI_SDC_GPIO_Port && pin == SPI_SDC_Pin));
    if (port == SSD2828_LCD_RESET_GPIO_Port && pin == SSD2828_LCD_RESET_Pin && !high) {
      ++model.hardware_resets; model.read_pending = false;
      model.registers[0xB7] = 0x0301u; model.registers[0xC0] = 0u;
      model.registers[0xC2] = model.registers[0xC3] = model.registers[0xC7] = 0u;
      model.registers[0xC6] = 0xC086u; model.registers[0xD4] = 0u;
    }
    if (port == PMU_EN2_GPIO_Port && pin == PMU_EN2_Pin) {
      if (high) model.power18_on = model.sequence;
      else assert((PMU_EN3_GPIO_Port->ODR & PMU_EN3_Pin) == 0u);
    }
    if (port == PMU_EN3_GPIO_Port && pin == PMU_EN3_Pin && high) {
      assert((PMU_EN2_GPIO_Port->ODR & PMU_EN2_Pin) != 0u);
      model.power12_on = model.sequence;
    }
    if (port == SSD2828_LCD_RESET_GPIO_Port && pin == SSD2828_LCD_RESET_Pin && high) {
      assert(model.tick >= 26u && model.power18_on < model.power12_on);
      assert((LCD_SHUT_GPIO_Port->ODR & LCD_SHUT_Pin) == 0u);
      model.reset_released = model.sequence;
    }
    if (port == PMU_EN1_GPIO_Port && pin == PMU_EN1_Pin && high) model.power28_tick = model.tick;
    if (port == LCD_RST_GPIO_Port && pin == LCD_RST_Pin && high) {
      assert((PMU_EN1_GPIO_Port->ODR & PMU_EN1_Pin) != 0u);
      assert(model.tick - model.power28_tick >= 20u);
    }
    if (port == PMU_EN4_GPIO_Port && pin == PMU_EN4_Pin && high) {
      assert(model.packet_count >= 39u && model.packets[model.packet_count - 1u][0] == 0x29u);
      assert(model.registers[0xB7] == 0x034Bu && model.registers[0xC2] == 0u);
      assert(model.registers[0xC3] == 0u && !model.read_pending);
      assert(model.tick - model.last_packet_tick >= 900u);
      assert(model.gpio_inits == 1u && (LCDBL_EN_GPIO_Port->ODR & LCDBL_EN_Pin) != 0u);
    }
  }
  if (high) port->ODR |= pin;
  else port->ODR &= ~(uint32_t)pin;
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
  if (port == SPI_MISO_GPIO_Port && pin == SPI_MISO_Pin)
    return (model.cs || WireRead(&model)) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  return (port->ODR & pin) != 0u ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

static void TestBoardPort(void)
{
  SSD2828_ProbeReport report;
  Reset();
  assert(BoardSSD2828_Probe(&report) == SSD2828_OK);
  assert(model.power18_on < model.power12_on && model.power12_on < model.reset_released);
  assert(strstr(uart_text, "[PASS] SSD2828") != NULL);
  Reset(); test_dwt.CYCCNT = 0xFFFFFE00u;
  assert(BoardSSD2828_Probe(&report) == SSD2828_OK);
  Reset(); model.ids[2] = 0u;
  assert(BoardSSD2828_Probe(&report) == SSD2828_ID_MISMATCH);
  assert((PMU_EN2_GPIO_Port->ODR & PMU_EN2_Pin) == 0u);
  assert((PMU_EN3_GPIO_Port->ODR & PMU_EN3_Pin) == 0u);
  assert((SSD2828_LCD_RESET_GPIO_Port->ODR & SSD2828_LCD_RESET_Pin) == 0u);
  assert((LCD_SHUT_GPIO_Port->ODR & LCD_SHUT_Pin) != 0u);
  for (unsigned fault = 0u; fault < 2u; ++fault) {
    Reset();
    model.ids[0] = model.ids[1] = model.ids[2] = fault == 0u ? 0xFFFFu : 0u;
    assert(BoardSSD2828_Probe(&report) == SSD2828_ID_MISMATCH);
    assert(strstr(uart_text, "before-poweroff GPIO-only EN1V8=1 EN1V2=1 RESET=1 SHUT=0") != NULL);
    assert((PMU_EN2_GPIO_Port->ODR & PMU_EN2_Pin) == 0u);
    assert((PMU_EN3_GPIO_Port->ODR & PMU_EN3_Pin) == 0u);
    BoardSSD2828_DisplayReport panel;
    assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_INVALID_ARGUMENT);
    assert(model.packet_count == 0u && model.gpio_inits == 0u);
  }
  Reset(); model.timer_running = false;
  assert(BoardSSD2828_Probe(&report) == SSD2828_TIMING_FAILED);
  assert(model.power18_on == 0u && strcmp(report.phase, "dwt-timer") == 0);
  Reset(); model.hclk = 179999999u;
  assert(BoardSSD2828_Probe(&report) == SSD2828_TIMING_FAILED);
  Reset(); test_ipsr = 3u;
  assert(BoardSSD2828_Probe(&report) == SSD2828_INVALID_ARGUMENT && model.gpio_writes == 0u);
  test_ipsr = 0u; test_primask = 1u;
  assert(BoardSSD2828_Probe(&report) == SSD2828_INVALID_ARGUMENT && model.gpio_writes == 0u);
  test_primask = 0u;
  puts("PASS board port: rail/reset order, failure shutdown, DWT stopped/wrap, IRQ guards");
}

static void TestPatterns(void)
{
  size_t count = DISPLAY_TEST_FRAME_BYTES / 2u;
  uint16_t *storage = malloc((count + 2u) * sizeof *storage);
  assert(storage != NULL && DISPLAY_TEST_FRAME_BYTES == 768000u && DISPLAY_TEST_STRIDE_BYTES == 800u);
  uint16_t *pixels = storage + 1u;
  storage[0] = 0x1357u; storage[count + 1u] = 0x2468u;
  const uint16_t colors[] = {0u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu};
  for (unsigned pattern = 0u; pattern < 5u; ++pattern) {
    assert(DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, (DisplayTest_Pattern)pattern));
    for (size_t i = 0u; i < count; ++i) assert(pixels[i] == colors[pattern]);
  }
  assert(DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_COLOR_BARS));
  const uint16_t bar_colors[] = {0xFFFFu, 0xFFE0u, 0x07FFu, 0x07E0u, 0xF81Fu, 0xF800u, 0x001Fu, 0u};
  for (unsigned bar = 0u; bar < 8u; ++bar) {
    assert(pixels[bar * 50u] == bar_colors[bar]);
    assert(pixels[959u * 400u + bar * 50u + 49u] == bar_colors[bar]);
  }
  assert(DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_CHECKERBOARD));
  assert(pixels[0] == 0xFFFFu && pixels[31] == 0xFFFFu && pixels[32] == 0u);
  assert(pixels[32u * 400u] == 0u && pixels[32u * 400u + 32u] == 0xFFFFu);
  assert(DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_GEOMETRY));
  assert(pixels[0] == 0xFFFFu && pixels[count - 1u] == 0xFFFFu);
  assert(pixels[401] == 0xF800u && pixels[798] == 0x07E0u);
  assert(pixels[958u * 400u + 1u] == 0x001Fu && pixels[958u * 400u + 398u] == 0xFFE0u);
  assert(pixels[200] == 0xFFFFu && pixels[100u * 400u + 200u] == 0xFFFFu);
  assert(pixels[100u * 400u + 100u] == 0x4208u);
  uint16_t saved = pixels[0];
  assert(!DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES - 1u, DISPLAY_PATTERN_RED));
  assert(!DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, (DisplayTest_Pattern)-1));
  assert(!DisplayTest_Draw(NULL, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_RED));
  assert(!DisplayTest_Draw((uint16_t *)((uint8_t *)pixels + 1u), DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_RED));
  assert(pixels[0] == saved && storage[0] == 0x1357u && storage[count + 1u] == 0x2468u);
  assert(strcmp(DisplayTest_PatternName(DISPLAY_PATTERN_COLOR_BARS), "color-bars") == 0);
  assert(strcmp(DisplayTest_PatternName(DISPLAY_PATTERN_COUNT), "invalid") == 0);
  assert(DisplayTest_Draw(pixels, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_HORIZONTAL_BARS));
  for (unsigned bar = 0u; bar < 8u; ++bar) {
    assert(pixels[bar * 120u * 400u] == bar_colors[bar]);
    assert(pixels[((bar + 1u) * 120u - 1u) * 400u + 399u] == bar_colors[bar]);
  }
  assert(storage[0] == 0x1357u && storage[count + 1u] == 0x2468u);
  free(storage);
  puts("PASS RGB565 nine patterns: solid/vertical/horizontal/checker/geometry, stride and guards");
}

static void TestLevelsAndDetail(void)
{
  size_t count = DISPLAY_TEST_FRAME_BYTES / 2u;
  uint16_t *storage = malloc((count + 2u) * sizeof *storage); assert(storage != NULL);
  uint16_t *p = storage + 1u;
  storage[0] = 0x1357u; storage[count + 1u] = 0x2468u;
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_GRAY16));
  unsigned previous_r = 0u, previous_g = 0u;
  for (unsigned i = 0u; i < 16u; ++i) {
    uint16_t c = p[500u * 400u + i * 25u];
    unsigned r = c >> 11u, g = (c >> 5u) & 63u, b = c & 31u;
    assert(r == b && (i == 0u || (r > previous_r && g > previous_g)));
    assert(p[500u * 400u + i * 25u + 24u] == c);
    previous_r = r; previous_g = g;
  }
  assert(p[500u * 400u] == 0u && p[500u * 400u + 399u] == 0xFFFFu);
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_RGB_LEVELS));
  const unsigned rows[] = {200u,500u,800u}, masks[] = {0xF800u,0x07E0u,0x001Fu};
  const unsigned shifts[] = {11u,5u,0u}, levels[] = {32u,64u,32u};
  for (unsigned band = 0u; band < 3u; ++band) {
    uint64_t seen = 0u;
    for (unsigned x = 0u; x < 400u; ++x) {
      uint16_t c = p[rows[band] * 400u + x];
      assert((c & ~masks[band]) == 0u);
      seen |= UINT64_C(1) << (c >> shifts[band]);
    }
    assert(seen == (levels[band] == 64u ? UINT64_MAX : UINT64_C(0xFFFFFFFF)));
    assert(p[rows[band] * 400u] == 0u && p[rows[band] * 400u + 399u] == masks[band]);
  }
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_RGB_BITS));
  for (unsigned bit = 0u; bit < 16u; ++bit) {
    uint16_t value = (uint16_t)(1u << bit);
    assert(p[300u * 400u + bit * 25u] == value);
    assert((uint16_t)(p[800u * 400u + bit * 25u] ^ value) == 0xFFFFu);
  }
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_CHECKER4));
  assert(p[3] == 0xFFFFu && p[4] == 0u && p[4u * 400u] == 0u && p[4u * 400u + 4u] == 0xFFFFu);
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_VERTICAL1));
  for (unsigned x = 0u; x < 400u; ++x) assert(p[700u * 400u + x] == ((x & 1u) ? 0u : 0xFFFFu));
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_HORIZONTAL1));
  for (unsigned y = 0u; y < 960u; ++y) assert(p[y * 400u + 123u] == ((y & 1u) ? 0u : 0xFFFFu));
  assert(DisplayTest_Draw(p, DISPLAY_TEST_FRAME_BYTES, DISPLAY_PATTERN_GEOMETRY_BODY));
  assert(p[160u * 400u + 20u] == 0xF800u && p[160u * 400u + 380u] == 0x07E0u);
  assert(p[940u * 400u + 20u] == 0x001Fu && p[940u * 400u + 380u] == 0xFFE0u);
  assert(p[552u * 400u + 100u] == 0xFFFFu && p[500u * 400u + 200u] == 0xFFFFu);
  uint32_t before = FrameHash(p);
  assert(!BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES, false, 0u, "BAD"));
  assert(!BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES, false, 100u, "BAD"));
  assert(!BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES, false, 1u, NULL));
  assert(!BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES - 1u, false, 1u, "BAD"));
  assert(FrameHash(p) == before);
  uint16_t *body = malloc((960u - 144u) * 400u * 2u); assert(body != NULL);
  memcpy(body, p + 144u * 400u, (960u - 144u) * 400u * 2u);
  assert(BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES, false, 12u, "HORIZONTAL 1"));
  assert(memcmp(body, p + 144u * 400u, (960u - 144u) * 400u * 2u) == 0);
  uint32_t rising = FrameHash(p);
  assert(BootScreen_DrawTestLabel(p, DISPLAY_TEST_FRAME_BYTES, true, 12u, "HORIZONTAL 1"));
  assert(FrameHash(p) != rising && p[50u * 400u + 350u] == 0u);
  assert(memcmp(body, p + 144u * 400u, (960u - 144u) * 400u * 2u) == 0);
  assert(storage[0] == 0x1357u && storage[count + 1u] == 0x2468u);
  free(body); free(storage);
  puts("PASS 16 grayscale levels, all 32/64/32 RGB levels, 16 isolated/inverse bits, checker4/one-pixel lines, bounded labels and body preservation");
}

static void SavePreview(const uint16_t *p, const char *filename)
{
  FILE *f = fopen(filename, "wb"); assert(f != NULL);
  fputs("P6\n400 960\n255\n", f);
  for (size_t i = 0u; i < DISPLAY_TEST_FRAME_BYTES / 2u; ++i) {
    fputc(((p[i] >> 11u) & 31u) * 255u / 31u, f);
    fputc(((p[i] >> 5u) & 63u) * 255u / 63u, f);
    fputc((p[i] & 31u) * 255u / 31u, f);
  }
  assert(fclose(f) == 0);
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio)
{
  assert(port == LCDBL_EN_GPIO_Port && gpio->Pin == LCDBL_EN_Pin);
  assert(gpio->Mode == GPIO_MODE_OUTPUT_PP && gpio->Pull == GPIO_NOPULL);
  assert((port->ODR & gpio->Pin) != 0u); /* ODR must be high before mode change. */
  ++model.gpio_inits;
}

HAL_StatusTypeDef HAL_LTDC_SetAddress_NoReload(LTDC_HandleTypeDef *ltdc, uint32_t address, uint32_t layer)
{
  assert(layer == 0u);
  if (model.reject_address) return HAL_ERROR;
  ltdc->address = address;
  return TestHAL_LTDC_SetAddress_NoReload(ltdc, address, layer);
}

static void SeedLayerConfig(LTDC_HandleTypeDef *ltdc)
{
  ltdc->Instance->BPCR = 0x001D001Fu;
  /* Same cached configuration as generated MX_LTDC_Init, deliberately CA=0405. */
  ltdc->LayerCfg[0] = (LTDC_LayerCfgTypeDef){
    .WindowX1 = 400u, .WindowY1 = 960u, .PixelFormat = 2u, .Alpha = 255u,
    .BlendingFactor1 = 0x400u, .BlendingFactor2 = 5u, .ImageWidth = 400u, .ImageHeight = 960u
  };
}

static void TestVendorAddressReplay(void)
{
  Reset();
  LTDC_TypeDef r = {0}; LTDC_HandleTypeDef ltdc = {.Instance = &r};
  SeedLayerConfig(&ltdc);
  ltdc.LayerCfg[0].Alpha0 = 0x12u;
  ltdc.LayerCfg[0].Backcolor = (LTDC_ColorTypeDef){.Red = 0x34u, .Green = 0x56u, .Blue = 0x78u};
  r.layers[0].BFCR = 0x607u; r.layers[0].DCCR = 0u;
  r.SRCR = 0u;
  assert(HAL_LTDC_SetAddress_NoReload(&ltdc, 0xC0000000u, 0u) == HAL_OK);
  assert(r.layers[0].BFCR == 0x405u && r.layers[0].DCCR == 0x12345678u);
  assert(r.layers[0].CACR == 255u && r.layers[0].PFCR == 2u);
  assert(r.layers[0].CFBAR == 0xC0000000u && r.layers[0].CFBLR == 0x03200323u);
  assert(r.layers[0].WHPCR == 0x01AD001Eu && r.layers[0].WVPCR == 0x03DF0020u);
  assert(r.layers[0].CFBLNR == 960u && (r.layers[0].CR & LTDC_LxCR_LEN) != 0u);
  assert(r.SRCR == 0u); /* NoReload changes shadow configuration, without committing it. */
  puts("PASS actual SDK HAL address update replays cached BFCR/DCCR/format/window/pitch and enables LEN without reload");
}

static void TestPacketsAndVideo(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  uint8_t bytes[1023];
  for (unsigned i = 0u; i < sizeof bytes; ++i) bytes[i] = (uint8_t)(i * 37u + 0xA5u);
  const uint16_t sizes[] = {1u, 2u, 53u, 1023u};
  for (unsigned i = 0u; i < 4u; ++i) {
    model.registers[0xBC] = sizes[i];
    assert(SSD2828_WritePacket(&bus, bytes, sizes[i]) == SSD2828_OK);
    assert(model.edges == 9u * (1u + sizes[i]));
    assert(model.lengths[i] == sizes[i] && memcmp(model.packets[i], bytes, sizes[i]) == 0);
  }
  assert(SSD2828_WritePacket(&bus, bytes, 1024u) == SSD2828_INVALID_ARGUMENT);
  assert(SSD2828_WritePacket(&bus, bytes, 0u) == SSD2828_INVALID_ARGUMENT);
  assert(SSD2828_WritePacket(&bus, NULL, 1u) == SSD2828_INVALID_ARGUMENT);
  Reset(); bus = Bus(); model.registers[0xBC] = 53u;
  assert(SSD2828_WritePacket(&bus, bytes, 53u) == SSD2828_OK);
  unsigned total = model.delay_calls;
  for (unsigned point = 2u; point <= total; ++point) {
    Reset(); bus = Bus(); model.registers[0xBC] = 53u; model.fail_delay = point;
    assert(SSD2828_WritePacket(&bus, bytes, 53u) == SSD2828_TIMING_FAILED);
    assert(model.cs && !model.clock);
  }
  Reset(); bus = Bus();
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK && video.ready);
  assert(model.registers[0xB2] == 0x201Eu && model.registers[0xB4] == 400u);
  assert(model.registers[0xB5] == 960u && model.registers[0xB6] == H863_DISPLAY_VICR6);
  assert(model.registers[0xBA] == 0x8332u && model.registers[0xBB] == 4u);
  assert(model.registers[0xDE] == 1u && model.registers[0xB7] == 0x0342u);
  uint32_t start = model.tick;
  model.block_cba = true;
  assert(SSD2828_VideoDCS(&video, bytes, 1u) == SSD2828_WAIT_TIMEOUT);
  assert(model.tick - start == 100u && model.packet_count == 0u);
  model.block_cba = false; model.block_cbe = true;
  assert(SSD2828_VideoDCS(&video, bytes, 1u) == SSD2828_WAIT_TIMEOUT);
  assert(model.packet_count == 1u && strcmp(video.phase, "dcs-buffer-drain") == 0);
  Reset(); bus = Bus(); video.bus = &bus; model.never_lock = true;
  assert(SSD2828_VideoConfigure(&video) == SSD2828_WAIT_TIMEOUT && !video.ready);
  assert(model.tick == 100u && strcmp(video.phase, "pll-lock") == 0);
  Reset(); bus = Bus(); video.bus = &bus; model.corrupt_reg = 0xB6u;
  assert(SSD2828_VideoConfigure(&video) == SSD2828_REGISTER_MISMATCH && !video.ready);
  puts("PASS exact FIFO lengths/no padding, all packet delay failures, video/readback, bounded PLL/buffer waits");
}

static void AssertPanelOff(void)
{
  assert((PMU_EN1_GPIO_Port->ODR & (PMU_EN1_Pin | PMU_EN4_Pin)) == 0u);
  assert((PMU_EN2_GPIO_Port->ODR & PMU_EN2_Pin) == 0u);
  assert((PMU_EN3_GPIO_Port->ODR & PMU_EN3_Pin) == 0u);
  assert((LCD_RST_GPIO_Port->ODR & LCD_RST_Pin) == 0u);
}

static void TestReadResponses(void)
{
  Reset(); SSD2828_Bus bus = Bus();
  uint8_t value = 0xA5u;
  assert(SSD2828_ReadReturnByte(&bus, &value) == SSD2828_WAIT_TIMEOUT);
  assert(value == 0xA5u && model.fifo_reads == 0u);
  model.registers[0xC6] |= 1u; model.rx_value = 0x69u;
  assert(SSD2828_ReadReturnByte(&bus, &value) == SSD2828_OK);
  assert(value == 0x69u && model.edges == 26u && model.fifo_reads == 1u);
  uint16_t word = 0x1234u;
  assert(SSD2828_ReadRegister(&bus, 0xFFu, &word) == SSD2828_INVALID_ARGUMENT);
  assert(word == 0x1234u);
  Reset(); bus = Bus(); model.registers[0xC6] |= 1u; model.rx_value = 0x69u;
  assert(SSD2828_ReadReturnByte(&bus, &value) == SSD2828_OK);
  unsigned total = model.delay_calls;
  for (unsigned point = 2u; point <= total; ++point) {
    Reset(); bus = Bus(); model.registers[0xC6] |= 1u; model.rx_value = 0x69u;
    model.fail_delay = point; value = 0xA5u;
    assert(SSD2828_ReadReturnByte(&bus, &value) == SSD2828_TIMING_FAILED);
    assert(value == 0xA5u && model.cs && !model.clock);
  }
  puts("PASS RX FIFO: RDR gate, exact 26 clocks, no extra byte, all delay failures preserve output and release CS");
  for (unsigned fault = 0u; fault < 10u; ++fault) {
    Reset(); bus = Bus(); SSD2828_Video v = {.bus = &bus, .delay_ms = HAL_Delay};
    assert(SSD2828_VideoConfigure(&v) == SSD2828_OK);
    model.registers[0xC7] = 0x0710u; /* Capture old errors then clear only documented bits. */
    model.rx_delay = 3u;
    if (fault == 1u) model.block_rdr = true;
    if (fault == 2u) model.rx_ack = 1u;
    if (fault == 3u) model.rx_errors = 0x400u;
    if (fault == 4u) model.rx_count = 0u;
    if (fault == 5u) model.rx_count = 2u;
    if (fault == 6u) model.block_btar = true;
    if (fault == 7u) model.stuck_cancel = true;
    if (fault == 8u) model.registers[0xC6] |= 1u; /* stale RDR must be cancelled */
    if (fault == 9u) model.rx_errors = 0x100u;
    SSD2828_ReadReport r;
    uint32_t start = model.tick;
    SSD2828_Status status = SSD2828_VideoReadDCS(&v, 0x0Cu, &r);
    assert(model.tick - start <= 105u && r.before_esr == 0x0710u);
    if (fault == 0u || fault == 8u) {
      assert(status == SSD2828_OK && r.valid && r.bytes_read == 1u && r.value == 0x50u);
      assert(r.count == 1u && r.esr == 0x10u && (r.isr & 1u) != 0u);
      assert((r.final_isr & 5u) == 4u && model.read_requests == 1u && model.fifo26_reads == 1u);
    } else {
      assert(status != SSD2828_OK && !r.valid);
      if (fault != 6u) assert(r.bytes_read == 0u && model.fifo_reads == 0u);
      if (fault == 2u) assert(r.ack == 1u);
      if (fault == 3u || fault == 9u) assert(r.esr == (uint16_t)(0x10u | model.rx_errors));
      if (fault == 4u || fault == 5u) assert(r.count == model.rx_count);
      if (fault == 1u || fault == 6u || fault == 7u) assert(status == SSD2828_WAIT_TIMEOUT);
    }
  }
  Reset(); bus = Bus(); SSD2828_Video v = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoConfigure(&v) == SSD2828_OK);
  assert(SSD2828_VideoEnable(&v) == SSD2828_OK);
  unsigned writes = model.writes;
  SSD2828_ReadReport r;
  assert(SSD2828_VideoReadDCS(&v, 0x0Cu, &r) == SSD2828_RESTART_REQUIRED);
  assert(model.writes == writes && model.video_pauses == 0u && model.read_requests == 0u);
  puts("PASS DCS read: fresh RDR, 50 byte, reserved bit4 retained, ACK/CRC/ECC/length/timeout/stale/BTA/COP faults, active-video refusal");
}

static void TestPanelEvidenceRecovery(void)
{
  for (unsigned fault = 0u; fault < 4u; ++fault) {
    Reset(); SSD2828_ProbeReport probe; BoardSSD2828_DisplayReport display;
    assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
    unsigned resets = model.hardware_resets;
    if (fault != 0u) model.block_rdr = true;
    if (fault == 2u) model.bad_restore_id = true;
    if (fault == 3u) model.rx_errors = 0x400u;
    SSD2828_Status status = BoardSSD2828_CollectPanelEvidence(&probe);
    assert(model.hardware_resets >= resets + 2u && !model.read_pending);
    assert(model.gpio_inits == 0u && (PMU_EN4_GPIO_Port->ODR & PMU_EN4_Pin) == 0u);
    assert(model.read_requests == (fault == 0u ? 3u : 1u));
    assert(strstr(uart_text, "[PANEL-READ] RESTORE controlled rails/reset cycle") != NULL);
    if (fault == 0u) {
      assert(model.fifo26_reads == 3u);
      assert(strstr(uart_text, "cmd=0x0C status=0 valid=1 bytes=1 value=50") != NULL);
      assert(strstr(uart_text, "cmd=0x0A status=0 valid=1 bytes=1 value=1C") != NULL);
      assert(strstr(uart_text, "cmd=0x0B status=0 valid=1 bytes=1 value=00") != NULL);
    }
    if (fault == 2u) {
      assert(status == SSD2828_ID_MISMATCH); AssertPanelOff(); continue;
    }
    assert(status == SSD2828_OK && probe.register_restored);
    assert(model.registers[0xC2] == 0u && model.registers[0xC3] == 0u);
    unsigned diagnostic_packets = model.packet_count;
    assert(diagnostic_packets == 39u + model.read_requests);
    assert(BoardSSD2828_StartPanel(true, &display) == SSD2828_OK);
    assert(model.packet_count == diagnostic_packets + 39u);
    assert(model.registers[0xB7] == 0x034Bu && model.registers[0xB6] == 0x2008u);
    assert(model.b6_video_on_writes == 0u && model.b6_backlight_on_writes == 0u);
    for (unsigned i = 0u; i < 39u; ++i) {
      unsigned packet = diagnostic_packets + i;
      assert(model.lengths[packet] == expected_commands[i].length);
      assert(memcmp(model.packets[packet], expected_commands[i].bytes, model.lengths[packet]) == 0);
    }
    BoardSSD2828_Shutdown(); AssertPanelOff();
  }
  puts("PASS boot evidence: 0C/0A/0B, first failure stops, full reset on success/failure, original 39-packet display restored, bad restore stays off");
}

static void TestPanelSequence(void)
{
  SSD2828_ProbeReport probe;
  BoardSSD2828_DisplayReport panel;
  Reset();
  assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
  unsigned writes = model.gpio_writes;
  assert(BoardSSD2828_StartPanel(false, &panel) == SSD2828_INVALID_ARGUMENT);
  assert(model.gpio_writes == writes && model.gpio_inits == 0u);
  uint32_t start = model.tick;
  assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_OK);
  assert(panel.commands_sent && panel.video_enabled && model.registers[0xB7] == 0x034Bu);
  /* V0.09 cold-start contract: select B before video, never by hot switching. */
  assert(model.registers[0xB6] == 0x2008u && model.reg_writes[0xB6] == 1u);
  assert(model.b6_video_on_writes == 0u && model.b6_backlight_on_writes == 0u);
  assert(model.tick - start == 1840u); /* rail20 + reset120 + 800+800 + frame100 */
  assert(model.packet_count == 39u);
  for (unsigned i = 0u; i < 39u; ++i) {
    assert(model.lengths[i] == expected_commands[i].length);
    assert(memcmp(model.packets[i], expected_commands[i].bytes, model.lengths[i]) == 0);
  }
  assert(model.packets[36][0] == 0x3Au && model.packets[36][1] == 0x50u);
  assert(model.gpio_inits == 1u && (PMU_EN4_GPIO_Port->ODR & PMU_EN4_Pin) != 0u);
  /* Every possible command completion can fail; none may enable the backlight. */
  for (unsigned command = 1u; command <= 39u; ++command) {
    Reset(); assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
    model.stop_after_packet = command;
    assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_WAIT_TIMEOUT);
    assert(!panel.video_enabled && model.gpio_inits == 0u);
    AssertPanelOff();
  }
  Reset(); assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
  model.never_lock = true;
  assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_WAIT_TIMEOUT);
  AssertPanelOff(); assert(model.packet_count == 0u && model.gpio_inits == 0u);
  Reset(); assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
  model.timer_running = false;
  assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_TIMING_FAILED);
  AssertPanelOff();
  puts("PASS cold FALLING: B6=2008 written once before VEN/backlight, no hot switch");
  puts("PASS 39 panel commands byte-for-byte vs factory, power/reset/delays, high-only PC10, all command failure shutdowns");
}

static uint32_t FrameHash(const uint16_t *pixels)
{
  uint32_t hash = 2166136261u;
  for (unsigned i = 0u; i < DISPLAY_TEST_FRAME_BYTES / 2u; ++i) hash = (hash ^ pixels[i]) * 16777619u;
  return hash;
}

static void TestScreen(void)
{
  const size_t count = DISPLAY_TEST_FRAME_BYTES / 2u;
  uint16_t *storage = calloc(count * 2u + 2u, sizeof *storage);
  assert(storage != NULL);
  storage[0] = 0x1357u; storage[count * 2u + 1u] = 0x2468u;
  uint16_t *first = storage + 1u, *second = first + count;
  assert(BootScreen_Draw(first, DISPLAY_TEST_FRAME_BYTES, 0u, false));
  uint32_t original = FrameHash(first);
  assert(first[0] == 0x07E0u && first[count - 1u] == 0x07E0u);
  assert(first[474u * 400u + 28u] == 0x07E0u);
  assert(!BootScreen_Draw(first, DISPLAY_TEST_FRAME_BYTES - 1u, 0u, false));
  assert(!BootScreen_Draw(NULL, DISPLAY_TEST_FRAME_BYTES, 0u, false));
  assert(!BootScreen_Draw((uint16_t *)((uint8_t *)first + 1u), DISPLAY_TEST_FRAME_BYTES, 0u, false));
  assert(FrameHash(first) == original);
  FILE *preview = fopen("screen_preview.ppm", "wb"); assert(preview != NULL);
  fputs("P6\n400 960\n255\n", preview);
  for (size_t i = 0u; i < count; ++i) {
    uint16_t pixel = first[i];
    fputc(((pixel >> 11u) & 31u) * 255u / 31u, preview);
    fputc(((pixel >> 5u) & 63u) * 255u / 63u, preview);
    fputc((pixel & 31u) * 255u / 31u, preview);
  }
  assert(fclose(preview) == 0);
  Reset();
  LTDC_TypeDef regs = {0}; LTDC_HandleTypeDef ltdc = {.Instance = &regs};
  SeedLayerConfig(&ltdc);
  BoardScreen screen;
  assert(!BoardScreen_Init(&screen, &ltdc, first, first + 1u));
  assert(BoardScreen_Init(&screen, &ltdc, first, second));
  assert(BoardScreen_Update(&screen, 1000u) && screen.pending && regs.SRCR == LTDC_SRCR_VBR);
  assert(ltdc.address == (uint32_t)(uintptr_t)second && FrameHash(first) == original);
  /* Active BoardScreen uses the CW90 layout: logical heartbeat (68,276)
     lands at native (123,68). Legacy BootScreen_Draw above stays portrait. */
  assert(second[68u * 400u + 123u] == 0x07E0u && FrameHash(second) != original);
  assert(second[24u * 400u + 123u] == 0x2104u);
  uint32_t back_hash = FrameHash(second);
  assert(!BoardScreen_Update(&screen, 2000u)); /* Stuck reload: don't write active frame. */
  assert(FrameHash(first) == original && FrameHash(second) == back_hash);
  regs.SRCR = 0u;
  assert(BoardScreen_Update(&screen, 2000u) && screen.front == 1u && screen.seconds == 2u);
  assert(first[0] == 0xF800u && FrameHash(second) == back_hash);
  regs.SRCR = 0u; regs.ISR = LTDC_ISR_FUIF | LTDC_ISR_TERRIF;
  assert(BoardScreen_Update(&screen, 3000u) && screen.errors == 1u);
  assert(regs.ICR == (LTDC_ICR_CFUIF | LTDC_ICR_CTERRIF) && second[0] == 0xF800u);
  regs.SRCR = 0u; regs.ISR = 0u; model.reject_address = true;
  assert(!BoardScreen_Update(&screen, 4000u) && !screen.pending);
  model.reject_address = false; model.tick = UINT32_MAX - 499u;
  assert(BoardScreen_Init(&screen, &ltdc, first, second));
  assert(BoardScreen_Update(&screen, 500u) && screen.seconds == 1u);
  assert(BootScreen_Draw(first, DISPLAY_TEST_FRAME_BYTES, UINT32_MAX, true));
  assert(storage[0] == 0x1357u && storage[count * 2u + 1u] == 0x2468u);
  free(storage);
  puts("PASS screen text/borders/colors/heartbeat, buffer guards, vblank ownership, stalled reload, scan errors and tick wrap");
}


static void TestSameEdgeDoesNotInterruptVideo(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
  assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
  unsigned writes = model.writes;
  uint32_t tick = model.tick;
  assert(SSD2828_VideoSetSamplingEdge(&video, H863_DISPLAY_SAMPLING_FALLING != 0) == SSD2828_OK);
  /* A request for the already active edge must not interrupt a good stream. */
  assert(model.writes == writes && model.tick == tick);
  assert(model.registers[0xB6] == H863_DISPLAY_VICR6 && model.registers[0xB7] == 0x034Bu);
  puts("PASS same-edge request: no video pause, register writes or frame delay");
}

static void TestSamplingEdge(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
  uint16_t before[256]; memcpy(before, model.registers, sizeof before);
  /* A different edge is a startup setting, while VEN is still off. */
  for (unsigned i = 0u; i < 4u; ++i) {
    bool falling = (i & 1u) == 0u;
    assert(SSD2828_VideoSetSamplingEdge(&video, falling) == SSD2828_OK);
    assert(model.registers[0xB6] == (falling ? 0x2008u : 0x0008u));
    assert(model.registers[0xB7] == 0x0342u);
    for (unsigned r = 0u; r < 256u; ++r)
      if (r != 0xB6u) assert(model.registers[r] == before[r]);
    assert(model.b6_video_on_writes == 0u);
  }
  assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
  unsigned writes = model.writes;
  uint32_t tick = model.tick;
  assert(SSD2828_VideoSetSamplingEdge(&video, true) == SSD2828_RESTART_REQUIRED);
  assert(strcmp(video.phase, "edge-restart-required") == 0);
  assert(model.writes == writes && model.tick == tick && model.video_pauses == 0u);
  assert(model.registers[0xB6] == 0x0008u && model.registers[0xB7] == 0x034Bu);
  /* Check all three register reads, including the command-stage write check. */
  const unsigned regs[] = {0xB7u,0xB6u,0xB6u};
  const unsigned reads[] = {1u,1u,2u};
  for (unsigned fault = 0u; fault < 3u; ++fault) {
    Reset(); bus = Bus(); video.bus = &bus;
    assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
    memset(model.reg_reads, 0, sizeof model.reg_reads);
    model.corrupt_reg = regs[fault]; model.corrupt_reg_read = reads[fault];
    assert(SSD2828_VideoSetSamplingEdge(&video, H863_DISPLAY_SAMPLING_FALLING == 0) == SSD2828_REGISTER_MISMATCH);
  }
  Reset(); bus = Bus(); video.bus = &bus;
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
  assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
  model.registers[0xB7] = 0x0343u; /* An unsupported partially paused state. */
  writes = model.writes;
  assert(SSD2828_VideoSetSamplingEdge(&video, false) == SSD2828_REGISTER_MISMATCH);
  assert(model.writes == writes);
  puts("PASS sampling edge: startup-only changes, hot change rejected without writes, readback failures, no implicit restart");
}

static void PrepareDiagnostic(BoardDisplayDiag *diag, LTDC_HandleTypeDef *ltdc,
                            LTDC_TypeDef *r, uint16_t *frame)
{
  Reset();
  *r = (LTDC_TypeDef){.GCR = LTDC_GCR_LTDCEN};
  *ltdc = (LTDC_HandleTypeDef){.Instance = r};
  SeedLayerConfig(ltdc);
  r->layers[0].CR = r->layers[1].CR = LTDC_LxCR_LEN;
  r->layers[0].CACR = r->layers[1].CACR = 255u;
  r->layers[0].BFCR = 0x405u; /* Actual CubeMX configuration, including the old BG bug. */
  r->layers[1].BFCR = 0x607u;
  model.scan_ltdc = ltdc;
  model.scan_frame = (uintptr_t)frame > 4096u ? frame : NULL;
  model.active_layer = true;
  if (model.scan_frame != NULL) model.active_hash = FrameHash(frame);
  SSD2828_ProbeReport probe;
  BoardSSD2828_DisplayReport panel;
  assert(BoardSSD2828_Probe(&probe) == SSD2828_OK);
  assert(BoardDisplayDiag_Prepare(diag, ltdc, frame, DISPLAY_TEST_FRAME_BYTES));
  assert(!model.active_layer && (r->layers[1].CR & LTDC_LxCR_LEN) == 0u);
  assert(BoardSSD2828_StartPanel(true, &panel) == SSD2828_OK);
}

static void StartDiagnostic(BoardDisplayDiag *diag, LTDC_HandleTypeDef *ltdc,
                            LTDC_TypeDef *r, uint16_t *frame)
{
  PrepareDiagnostic(diag, ltdc, r, frame);
  unsigned writes = model.writes, control_writes = model.sequence;
  assert(BoardDisplayDiag_Start(diag));
  assert(model.writes == writes && model.sequence == control_writes);
  assert(model.active_layer && model.active_background == 0u && diag->step == 0u);
}

static void AdvanceDiagnostic(BoardDisplayDiag *s, uint32_t elapsed)
{
  model.tick = s->started + elapsed;
  assert(BoardDisplayDiag_Update(s, model.tick));
}

static void TestDiagnosticLoop(void)
{
  size_t count = DISPLAY_TEST_FRAME_BYTES / 2u;
  uint16_t *storage = calloc(count + 2u, sizeof *storage); assert(storage != NULL);
  uint16_t *frame = storage + 1u;
  storage[0] = 0x1357u; storage[count + 1u] = 0x2468u;
  /* Regression reproducer: LEN=0 is insufficient when CA=255 ignores DCCR alpha. */
  LTDC_Layer_TypeDef old = {.BFCR=0x405u, .CACR=255u, .DCCR=0u};
  assert(BlendDefault(&old, 0xFFFFFFu) == 0u);
  old.BFCR = 0x607u;
  assert(BlendDefault(&old, 0xFFFFFFu) == 0xFFFFFFu);
  LTDC_TypeDef r; LTDC_HandleTypeDef ltdc; BoardDisplayDiag diag;
  StartDiagnostic(&diag, &ltdc, &r, frame);
  assert(DISPLAY_DIAG_HOLD_MS == 4000u && DISPLAY_DIAG_STEPS_PER_CYCLE == 8u);
  unsigned writes = model.writes, control_writes = model.sequence;
  unsigned b6_writes = model.reg_writes[0xB6], b7_writes = model.reg_writes[0xB7];
  uint32_t page_hash[2] = {0};
  for (unsigned i = 0u; i < 16u; ++i) {
    unsigned step = i % 8u;
    assert(diag.step == step && diag.falling && !diag.bist_active);
    assert(model.registers[0xB6] == 0x2008u && model.registers[0xB7] == 0x034Bu);
    assert((r.layers[1].CR & LTDC_LxCR_LEN) == 0u && r.SRCR == 0u);
    assert(r.layers[0].BFCR == 0x607u && r.layers[1].BFCR == 0x607u);
    assert(r.layers[0].DCCR == 0u && r.layers[1].DCCR == 0u);
    for (unsigned layer = 0u; layer < 2u; ++layer) {
      assert(ltdc.LayerCfg[layer].BlendingFactor1 == 0x600u &&
             ltdc.LayerCfg[layer].BlendingFactor2 == 7u && ltdc.LayerCfg[layer].Alpha0 == 0u);
      assert(ltdc.LayerCfg[layer].Backcolor.Red == 0u &&
             ltdc.LayerCfg[layer].Backcolor.Green == 0u && ltdc.LayerCfg[layer].Backcolor.Blue == 0u);
    }
    if (step == DISPLAY_DIAG_BG_STEP) {
      assert(!model.active_layer && model.active_background == 0xFFFFFFu && model.active_rgb == 0xFFFFFFu);
    } else {
      assert(model.active_layer && model.active_address == (uint32_t)(uintptr_t)frame);
      assert(r.layers[0].PFCR == 2u && r.layers[0].CACR == 255u &&
             r.layers[0].CFBLR == 0x03200323u && r.layers[0].CFBLNR == 960u &&
             r.layers[0].WHPCR == 0x01AD001Eu && r.layers[0].WVPCR == 0x03DF0020u);
      if (step == DISPLAY_DIAG_FB_WHITE_STEP)
        for (size_t k = 0u; k < count; ++k) assert(frame[k] == 0xFFFFu);
      if (step == DISPLAY_DIAG_GEOMETRY_STEP) {
        assert(frame[0] == 0xFFFFu && frame[count - 1u] == 0xFFFFu);
        /* Physical corners after clockwise logical 960x400 rotation. */
        assert(H863_DISPLAY_ROTATION == 90u);
        assert(frame[401] == 0x001Fu && frame[798] == 0xF800u);
        assert(frame[958u * 400u + 1u] == 0xFFE0u && frame[958u * 400u + 398u] == 0x07E0u);
      }
      if (step == 0u) page_hash[i / 8u] = FrameHash(frame);
      char filename[64];
      snprintf(filename, sizeof filename, "page_%c_%02u.ppm", diag.falling ? 'B' : 'A', step + 1u);
      SavePreview(frame, filename);
    }
    AdvanceDiagnostic(&diag, 999u); assert(!diag.sampled);
    model.registers[0xC7] = 0x10u; /* Reserved bit preserved, no panel ACK claimed. */
    AdvanceDiagnostic(&diag, 1000u); assert(diag.sampled && model.registers[0xC7] == 0x10u);
    AdvanceDiagnostic(&diag, 3999u); assert(diag.step == step);
    AdvanceDiagnostic(&diag, 4000u);
    assert(model.active_frame_violations == 0u && diag.errors == 0u);
    assert(storage[0] == 0x1357u && storage[count + 1u] == 0x2468u);
  }
  assert(diag.cycle == 3u && diag.step == 0u && diag.falling);
  assert(page_hash[0] == page_hash[1]);
  assert(model.packet_count == 39u && model.gpio_inits == 1u);
  assert(model.b6_video_on_writes == 0u && model.b6_backlight_on_writes == 0u);
  assert(model.writes == writes && model.sequence == control_writes);
  assert(model.reg_writes[0xB6] == b6_writes && model.reg_writes[0xB7] == b7_writes);
  assert(model.reg_writes[0xEE] == 0u && model.video_pauses == 0u);
  assert(model.reg_reads[0xED] == 16u);
  assert(strstr(uart_text, "source=BG") && strstr(uart_text, "source=FB") &&
         strstr(uart_text, "C7=0010") && strstr(uart_text, "unchanged=1"));
  BoardSSD2828_DisplayReport report;
  assert(BoardSSD2828_SetSamplingEdge(true, &report) == SSD2828_OK);
  assert(BoardSSD2828_SetSamplingEdge(false, &report) == SSD2828_RESTART_REQUIRED);
  assert(model.writes == writes && model.sequence == control_writes);
  assert((PMU_EN4_GPIO_Port->ODR & PMU_EN4_Pin) != 0u);
  /* Thirty minutes of simulated page time: verify that later loop wraps never
     introduce a video pause or BIST. This is not a hardware endurance test. */
  for (unsigned page = 0u; page < 450u; ++page) {
    if (uart_length > sizeof uart_text / 2u) uart_text[uart_length = 0u] = '\0';
    AdvanceDiagnostic(&diag, 4000u);
    assert(diag.falling && !diag.bist_active && diag.errors == 0u);
    assert(model.writes == writes && model.sequence == control_writes);
    assert(model.video_pauses == 0u && model.reg_writes[0xEE] == 0u);
  }
  /* BG cannot access the frame, even if passed an invalid aligned pointer. */
  while (diag.step != DISPLAY_DIAG_BG_STEP) AdvanceDiagnostic(&diag, 4000u);
  assert(diag.step == DISPLAY_DIAG_BG_STEP && !model.active_layer);
  diag.frame = (uint16_t *)(uintptr_t)2u; model.scan_frame = NULL;
  AdvanceDiagnostic(&diag, 1000u);
  AdvanceDiagnostic(&diag, 3999u);
  assert(model.active_rgb == 0xFFFFFFu);
  free(storage);
  puts("PASS fixed FALLING: two 8-page cycles, 4s holds, exact raw FB/BG white, full corners, no B6/B7/EE writes or backlight toggles, no BG frame access, VBlank ownership");
}

static void AssertDiagnosticStopped(BoardDisplayDiag *s)
{
  assert(!s->running && !s->prepared && s->errors == 1u);
  assert((s->ltdc->Instance->GCR & LTDC_GCR_LTDCEN) == 0u);
  AssertPanelOff();
  assert(!BoardDisplayDiag_Update(s, model.tick));
  assert(s->errors == 1u); /* A stopped diagnostic must not repeatedly fail. */
}

static void TestDiagnosticFailures(void)
{
  uint16_t *frame = calloc(1u, DISPLAY_TEST_FRAME_BYTES); assert(frame != NULL);
  LTDC_TypeDef r; LTDC_HandleTypeDef ltdc; BoardDisplayDiag diag;
  assert(!BoardDisplayDiag_Prepare(NULL, &ltdc, frame, DISPLAY_TEST_FRAME_BYTES));
  for (unsigned fault = 0u; fault < 10u; ++fault) {
    StartDiagnostic(&diag, &ltdc, &r, frame);
    switch (fault) {
      case 0u: r.ISR = LTDC_ISR_FUIF | LTDC_ISR_TERRIF; break;
      case 1u: model.timer_running = false; break;
      case 2u: model.corrupt_reg = 0xB6u; break;
      case 3u: model.never_lock = true; break;
      case 4u: model.stuck_reload = true; break;
      case 5u: frame[12345] ^= 1u; break;
      case 6u: model.reject_address = true; break;
      case 7u: model.registers[0xB7] = 0x0343u; break;
      case 8u: r.layers[0].BFCR = 0x405u; break;
      default: model.registers[0xEE] = 0x0200u; break;
    }
    model.tick = diag.started + (fault == 4u || fault == 6u ? 4000u : 1000u);
    uint32_t before = model.tick;
    assert(!BoardDisplayDiag_Update(&diag, model.tick));
    AssertDiagnosticStopped(&diag);
    assert(model.tick - before <= 200u);
    assert(strstr(uart_text, "[DIAG][FAIL]") != NULL);
    if (fault == 0u) assert(diag.last_isr == (LTDC_ISR_FUIF | LTDC_ISR_TERRIF));
    if (fault == 5u) assert(strcmp(diag.phase, "diag-frame-changed") == 0);
    if (fault == 7u || fault == 9u) assert(strcmp(diag.phase, "diag-bridge-state") == 0);
    if (fault == 8u) assert(strcmp(diag.phase, "diag-layer-state") == 0);
  }
  StartDiagnostic(&diag, &ltdc, &r, frame);
  diag.started = model.tick = UINT32_MAX - 499u;
  model.tick = 500u;
  assert(BoardDisplayDiag_Update(&diag, model.tick) && diag.step == 0u && diag.sampled);
  model.tick = diag.started + 4000u;
  assert(BoardDisplayDiag_Update(&diag, model.tick) && diag.step == 1u);
  free(frame);
  puts("PASS diagnostic scan/clock/readback/PLL/VBlank/frame/address/edge/blend failures, bounded shutdown/latch and tick wrap");
}

static void TestBridgeBIST(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoSetBIST(NULL, true) == SSD2828_INVALID_ARGUMENT);
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
  assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
  model.registers[0xEE] = 0xF0A5u; /* RO status plus existing selector/command. */
  uint16_t before[256]; memcpy(before, model.registers, sizeof before);
  assert(SSD2828_VideoSetBIST(&video, true) == SSD2828_OK);
  assert((model.registers[0xEE] & 0x1FFFu) == 0x16A5u);
  assert(SSD2828_VideoSetBIST(&video, false) == SSD2828_OK);
  assert(model.bist_write_count == 4u);
  const uint16_t expected[] = {0x12A5u,0x16A5u,0x12A5u,0x10A5u};
  for (unsigned i = 0u; i < 4u; ++i) {
    assert(model.bist_writes[i] == expected[i]);
    assert((model.bist_writes[i] & 0xE900u) == 0u); /* No RO replay or calibration. */
  }
  assert(model.bist_write_ticks[3] - model.bist_write_ticks[2] >= 20u);
  for (unsigned r = 0u; r < 256u; ++r)
    if (r != 0xEEu) assert(model.registers[r] == before[r]);
  /* Each control write readback may fail; no false success on stuck bits. */
  for (unsigned read = 2u; read <= 6u; ++read) {
    if (read == 4u) continue; /* Initial read on exit is covered by state guards. */
    Reset(); bus = Bus(); video.bus = &bus;
    assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
    assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
    model.corrupt_reg = 0xEEu; model.corrupt_reg_read = read;
    SSD2828_Status st = SSD2828_VideoSetBIST(&video, true);
    if (read <= 3u) assert(st == SSD2828_REGISTER_MISMATCH);
    else {
      assert(st == SSD2828_OK);
      assert(SSD2828_VideoSetBIST(&video, false) == SSD2828_REGISTER_MISMATCH);
    }
  }
  for (unsigned bits = 0u; bits < 2u; ++bits) {
    Reset(); bus = Bus(); video.bus = &bus;
    assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
    assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
    model.registers[0xEE] = bits == 0u ? 0x0800u : 0x0100u;
    assert(SSD2828_VideoSetBIST(&video, true) == SSD2828_REGISTER_MISMATCH);
    assert(model.bist_write_count == 0u);
  }
  size_t count = DISPLAY_TEST_FRAME_BYTES / 2u;
  uint16_t *frame = calloc(count, sizeof *frame); assert(frame != NULL);
  LTDC_TypeDef r; LTDC_HandleTypeDef ltdc; BoardDisplayDiag diag;
  PrepareDiagnostic(&diag, &ltdc, &r, (uint16_t *)(uintptr_t)2u);
  assert(BoardDisplayDiag_StartBridgeBIST(&diag));
  assert(diag.bist_active && !model.active_layer && diag.frame_hash == 0u);
  assert(model.registers[0xEE] == 0x0600u && model.packet_count == 39u);
  /* Snapshot must not dereference this invalid aligned framebuffer in BIST. */
  AdvanceDiagnostic(&diag, 1000u);
  AdvanceDiagnostic(&diag, DISPLAY_DIAG_BIST_HOLD_MS - 1u);
  assert(diag.bist_active && diag.step == 0u);
  diag.frame = frame; model.scan_frame = frame;
  AdvanceDiagnostic(&diag, DISPLAY_DIAG_BIST_HOLD_MS);
  assert(!diag.bist_active && model.registers[0xEE] == 0u && model.active_layer);
  assert(diag.step == 0u && diag.falling && diag.errors == 0u);
  assert(strstr(uart_text, "[DIAG][BIST] START") && strstr(uart_text, "[DIAG][BIST] END"));
  assert(model.packet_count == 39u && model.active_frame_violations == 0u);
  for (unsigned fault = 0u; fault < 4u; ++fault) {
    PrepareDiagnostic(&diag, &ltdc, &r, frame);
    if (fault == 0u) {
      model.corrupt_reg = 0xEEu;
      assert(!BoardDisplayDiag_StartBridgeBIST(&diag));
    } else {
      assert(BoardDisplayDiag_StartBridgeBIST(&diag));
      if (fault == 1u) model.registers[0xEE] = 0u; /* BIST unexpectedly stopped. */
      if (fault == 2u) model.never_lock = true;
      if (fault == 3u) {
        AdvanceDiagnostic(&diag, 1000u);
        model.corrupt_reg = 0xEEu;
        model.corrupt_reg_read = model.reg_reads[0xEE] + 2u; /* stop readback */
      }
      model.tick = diag.started + (fault == 3u ? DISPLAY_DIAG_BIST_HOLD_MS : 1000u);
      assert(!BoardDisplayDiag_Update(&diag, model.tick));
    }
    AssertDiagnosticStopped(&diag);
  }
  PrepareDiagnostic(&diag, &ltdc, &r, frame);
  assert(BoardDisplayDiag_StartBridgeBIST(&diag));
  diag.started = model.tick = UINT32_MAX - 499u;
  AdvanceDiagnostic(&diag, DISPLAY_DIAG_BIST_HOLD_MS);
  assert(!diag.bist_active && diag.step == 0u && diag.errors == 0u);
  free(frame);
  puts("PASS bridge BIST: internal-source interval without frame access, EE field protection, read-clear flags, stop/frame-wait/disable, readback/PLL failures, shutdown, RGB return and tick wrap");
}

int main(int argc, char **argv)
{
  UART_HandleTypeDef uart = {0};
  DebugLog_Init(&uart);
  if (argc == 2 && strcmp(argv[1], "--expect-v004-failure") == 0) {
    uint16_t *frame = calloc(1u, DISPLAY_TEST_FRAME_BYTES); assert(frame != NULL);
    LTDC_TypeDef r; LTDC_HandleTypeDef ltdc; BoardDisplayDiag diag;
    StartDiagnostic(&diag, &ltdc, &r, frame);
    assert(r.layers[0].BFCR == 0x405u && r.layers[1].BFCR == 0x607u);
    assert(diag.frame_hash == 0xCA38CAD1u); /* Exact first-page hash from 17:48/18:00 captures. */
    model.tick = diag.started + 1000u;
    assert(!BoardDisplayDiag_Update(&diag, model.tick));
    AssertDiagnosticStopped(&diag);
    assert(strcmp(diag.phase, "diag-layer-state") == 0);
    assert(strstr(uart_text, "[DIAG][FAIL] phase=diag-layer-state") != NULL);
    free(frame);
    puts("REPRODUCED V0.04 hardware log: first-page BFCR0405/0607, diag-layer-state, LTDC OFF, error latched, poweroff path");
    return 0;
  }
  TestProtocol();
  TestProbe();
  TestBoardPort();
  TestPatterns();
  TestLevelsAndDetail();
  TestReadResponses();
  TestPanelEvidenceRecovery();
  TestPacketsAndVideo();
  TestPanelSequence();
  TestScreen();
  TestSameEdgeDoesNotInterruptVideo();
  TestSamplingEdge();
  TestVendorAddressReplay();
  TestDiagnosticLoop();
  TestDiagnosticFailures();
  TestBridgeBIST();
  puts("ALL DISPLAY TESTS PASSED");
  return 0;
}
