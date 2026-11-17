#include "ssd2828_bus.h"
#include "gc9503_panel.h"
#include "board_screen.h"
#include "board_display_diag.h"
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
  unsigned corrupt_reg_read, reg_reads[256], b6_video_on_writes, b6_backlight_on_writes;
  LTDC_HandleTypeDef *scan_ltdc;
  uint16_t *scan_frame;
  uint32_t active_hash, active_address, active_background;
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
        model.registers[reg] = (uint16_t)(Decode(10u, 8u) | (Decode(19u, 8u) << 8u));
        ++model.writes;
      } else if (model.edges == 34u) {
        assert(!model.bits[0] && !model.bits[9]);
        assert(Decode(10u, 8u) == 0xFAu);
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
    model.returned = model.registers[reg];
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
      model.returned = model.ids[model.id_reads % 3u];
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
  model.registers[0xC6] = 0xC080u;
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
      assert(model.packet_count == 39u && model.packets[38][0] == 0x29u);
      assert(model.tick - model.last_packet_tick >= 900u);
      assert(model.gpio_inits == 1u && (LCDBL_EN_GPIO_Port->ODR & LCDBL_EN_Pin) != 0u);
    }
  }
  if (high) port->ODR |= pin;
  else port->ODR &= ~(uint32_t)pin;
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
  assert(port == SPI_MISO_GPIO_Port && pin == SPI_MISO_Pin);
  return WireRead(&model) ? GPIO_PIN_SET : GPIO_PIN_RESET;
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
  LTDC_LAYER(ltdc, 0u)->CFBAR = address;
  return HAL_OK;
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
  assert(model.registers[0xB5] == 960u && model.registers[0xB6] == 8u);
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
  BoardScreen screen;
  assert(!BoardScreen_Init(&screen, &ltdc, first, first + 1u));
  assert(BoardScreen_Init(&screen, &ltdc, first, second));
  assert(BoardScreen_Update(&screen, 1000u) && screen.pending && regs.SRCR == LTDC_SRCR_VBR);
  assert(ltdc.address == (uint32_t)(uintptr_t)second && FrameHash(first) == original);
  assert(second[474u * 400u + 72u] == 0x07E0u && FrameHash(second) != original);
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


static void TestSamplingEdge(void)
{
  Reset();
  SSD2828_Bus bus = Bus();
  SSD2828_Video video = {.bus = &bus, .delay_ms = HAL_Delay};
  assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
  assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
  uint16_t before[256]; memcpy(before, model.registers, sizeof before);
  for (unsigned i = 0u; i < 4u; ++i) {
    bool falling = (i & 1u) == 0u;
    assert(SSD2828_VideoSetSamplingEdge(&video, falling) == SSD2828_OK);
    assert(model.registers[0xB6] == (falling ? 0x2008u : 0x0008u));
    assert(model.registers[0xB7] == 0x034Bu);
    for (unsigned r = 0u; r < 256u; ++r)
      if (r != 0xB6u) assert(model.registers[r] == before[r]);
    assert(model.b6_video_on_writes == 0u);
  }
  /* Every config/readback phase may fail, including the resume write. */
  const unsigned regs[] = {0xB7u,0xB7u,0xB6u,0xB6u,0xB7u};
  const unsigned reads[] = {1u,2u,1u,2u,3u};
  for (unsigned fault = 0u; fault < 5u; ++fault) {
    Reset(); bus = Bus(); video.bus = &bus;
    assert(SSD2828_VideoConfigure(&video) == SSD2828_OK);
    assert(SSD2828_VideoEnable(&video) == SSD2828_OK);
    memset(model.reg_reads, 0, sizeof model.reg_reads);
    model.corrupt_reg = regs[fault]; model.corrupt_reg_read = reads[fault];
    assert(SSD2828_VideoSetSamplingEdge(&video, true) == SSD2828_REGISTER_MISMATCH);
  }
  puts("PASS sampling edge: only B6 bit13 changes, VEN paused, all five read/check failure phases");
}

static void StartDiagnostic(BoardDisplayDiag *diag, LTDC_HandleTypeDef *ltdc,
                            LTDC_TypeDef *r, uint16_t *frame)
{
  Reset();
  *r = (LTDC_TypeDef){.GCR = LTDC_GCR_LTDCEN};
  *ltdc = (LTDC_HandleTypeDef){.Instance = r};
  r->layers[0].CR = r->layers[1].CR = LTDC_LxCR_LEN;
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
  assert(BoardDisplayDiag_Start(diag));
  assert(!model.active_layer && model.active_background == 0u && diag->step == 0u);
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
  LTDC_TypeDef r; LTDC_HandleTypeDef ltdc; BoardDisplayDiag diag;
  StartDiagnostic(&diag, &ltdc, &r, frame);
  const uint32_t backgrounds[] = {0u,0xFFFFFFu,0xFF0000u,0x00FF00u,0x0000FFu};
  const uint16_t solids[] = {0u,0xFFFFu,0xF800u,0x07E0u,0x001Fu};
  uint32_t page_hash[2] = {0};
  for (unsigned i = 0u; i < 30u; ++i) {
    unsigned step = i % 15u;
    assert(diag.step == step && diag.falling == (i >= 15u));
    assert(model.registers[0xB6] == (diag.falling ? 0x2008u : 0x0008u));
    assert((r.layers[1].CR & LTDC_LxCR_LEN) == 0u && r.SRCR == 0u);
    if (step < 5u) {
      assert(!model.active_layer && model.active_background == backgrounds[step]);
    } else {
      assert(model.active_layer && model.active_address == (uint32_t)(uintptr_t)frame);
      if (step < 10u) for (size_t k = 0u; k < count; ++k) assert(frame[k] == solids[step - 5u]);
      if (step == 10u) assert(frame[49] == 0xFFFFu && frame[50] == 0xFFE0u);
      if (step == 11u) assert(frame[119u * 400u + 399u] == 0xFFFFu && frame[120u * 400u] == 0xFFE0u);
      if (step == 12u) assert(frame[31] == 0xFFFFu && frame[32] == 0u);
      if (step == 13u) assert(frame[0] == 0xFFFFu && frame[401] == 0xF800u);
      if (step == 14u) {
        page_hash[i / 15u] = FrameHash(frame);
        char filename[64]; snprintf(filename, sizeof filename, "diagnostic_%s.ppm", diag.falling ? "falling" : "rising");
        FILE *preview = fopen(filename, "wb"); assert(preview != NULL);
        fputs("P6\n400 960\n255\n", preview);
        for (size_t k = 0u; k < count; ++k) {
          uint16_t pixel = frame[k];
          fputc(((pixel >> 11u) & 31u) * 255u / 31u, preview);
          fputc(((pixel >> 5u) & 63u) * 255u / 63u, preview);
          fputc((pixel & 31u) * 255u / 31u, preview);
        }
        assert(fclose(preview) == 0);
      }
    }
    AdvanceDiagnostic(&diag, 999u); assert(!diag.sampled);
    /* ESR is diagnostic evidence, not an acknowledgement or automatic stop. */
    model.registers[0xC7] = 1u;
    AdvanceDiagnostic(&diag, 1000u); assert(diag.sampled && model.registers[0xC7] == 1u);
    uint32_t duration = step == 14u ? 10000u : 5000u;
    AdvanceDiagnostic(&diag, duration - 1u); assert(diag.step == step);
    AdvanceDiagnostic(&diag, duration);
    assert(model.active_frame_violations == 0u && diag.errors == 0u);
    assert(storage[0] == 0x1357u && storage[count + 1u] == 0x2468u);
  }
  assert(diag.cycle == 2u && diag.step == 0u && !diag.falling);
  assert(page_hash[0] != page_hash[1]);
  assert(model.packet_count == 39u && model.gpio_inits == 1u);
  assert(model.b6_video_on_writes == 0u && model.b6_backlight_on_writes == 0u);
  assert(strstr(uart_text, "source=BG") && strstr(uart_text, "source=FB") &&
         strstr(uart_text, "C7=0001") && strstr(uart_text, "unchanged=1"));
  /* Prove BG phases do not dereference frame: invalid but aligned pointer under ASan. */
  StartDiagnostic(&diag, &ltdc, &r, (uint16_t *)(uintptr_t)2u);
  for (unsigned step = 0u; step < 5u; ++step) {
    AdvanceDiagnostic(&diag, 1000u);
    if (step < 4u) AdvanceDiagnostic(&diag, 5000u);
  }
  assert(!model.active_layer && diag.step == 4u);
  free(storage);
  puts("PASS diagnostic 30-step/two-edge loop, no BG frame access, VBlank ownership, same solids, patterns/page, sticky ESR, guard bytes");
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
  for (unsigned fault = 0u; fault < 8u; ++fault) {
    StartDiagnostic(&diag, &ltdc, &r, frame);
    switch (fault) {
      case 0u: r.ISR = LTDC_ISR_FUIF | LTDC_ISR_TERRIF; break;
      case 1u: model.timer_running = false; break;
      case 2u: model.corrupt_reg = 0xB6u; break;
      case 3u: model.never_lock = true; break;
      case 4u: model.stuck_reload = true; break;
      case 5u: /* Enter FB then flip memory after baseline check. */
        for (unsigned step = 0u; step < 5u; ++step) AdvanceDiagnostic(&diag, 5000u);
        frame[12345] ^= 1u; break;
      case 6u:
        for (unsigned step = 0u; step < 4u; ++step) AdvanceDiagnostic(&diag, 5000u);
        model.reject_address = true; break;
      default:
        /* Last RISING page -> FALLING edge readback failure. */
        for (unsigned step = 0u; step < 14u; ++step) AdvanceDiagnostic(&diag, 5000u);
        AdvanceDiagnostic(&diag, 1000u);
        model.corrupt_reg = 0xB6u;
        model.corrupt_reg_read = model.reg_reads[0xB6] + 2u; /* read OK, writeback bad */
        break;
    }
    model.tick = diag.started + (fault == 7u ? 10000u : fault == 4u || fault == 6u ? 5000u : 1000u);
    uint32_t before = model.tick;
    assert(!BoardDisplayDiag_Update(&diag, model.tick));
    AssertDiagnosticStopped(&diag);
    assert(model.tick - before <= 200u); /* Counted VBlank wait, shutdown reset hold. */
    assert(strstr(uart_text, "[DIAG][FAIL]") != NULL);
    if (fault == 0u) assert(diag.last_isr == (LTDC_ISR_FUIF | LTDC_ISR_TERRIF));
    if (fault == 5u) assert(strcmp(diag.phase, "diag-frame-changed") == 0);
    if (fault == 7u) assert(strcmp(diag.phase, "edge-write-vicr6") == 0);
  }
  StartDiagnostic(&diag, &ltdc, &r, frame);
  diag.started = model.tick = UINT32_MAX - 499u;
  model.tick = 500u;
  assert(BoardDisplayDiag_Update(&diag, model.tick) && diag.step == 0u && diag.sampled);
  model.tick = diag.started + 5000u;
  assert(BoardDisplayDiag_Update(&diag, model.tick) && diag.step == 1u);
  free(frame);
  puts("PASS diagnostic scan/clock/readback/PLL/VBlank/frame/address/edge failures, shutdown/latch/bounds and tick wrap");
}

int main(void)
{
  UART_HandleTypeDef uart = {0};
  DebugLog_Init(&uart);
  TestProtocol();
  TestProbe();
  TestBoardPort();
  TestPatterns();
  TestPacketsAndVideo();
  TestPanelSequence();
  TestScreen();
  TestSamplingEdge();
  TestDiagnosticLoop();
  TestDiagnosticFailures();
  puts("ALL DISPLAY TESTS PASSED");
  return 0;
}
