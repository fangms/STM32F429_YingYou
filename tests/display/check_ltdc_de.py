"""Exercise real main.c DE override/telemetry; the model does not measure a pad."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

firmware = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
assert output != firmware and firmware not in output.parents
output.mkdir(parents=True, exist_ok=True)
main = firmware / 'Core/Src/main.c'
source = main.read_text(encoding='utf-8')
blocks = dict(re.findall(r'/\* USER CODE BEGIN ([^\r\n]+?) \*/(.*?)/\* USER CODE END \1 \*/', source, re.S))
helper = blocks['0']
override = blocks['LTDC_Init 2']
assert 'DisplayDE_LogPad("generated")' in override, 'Missing before/after DE evidence'
assert 'DisplayDE_LogPad("ssd2828")' in override, 'Missing physical DE verification'
assert 'hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;' in override
assert 'CLEAR_BIT(hltdc.Instance->GCR, LTDC_GCR_DEPOL);' in override
assert source.index('MX_LTDC_Init();') < source.index('BoardSSD2828_StartPanel(true,')

device = firmware / 'Drivers/CMSIS/Device/ST/STM32F4xx/Include/stm32f429xx.h'
hal = firmware / 'Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_ltdc.h'
gpio = firmware / 'Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_gpio.h'
definitions = []
for path, names in [
    (device, ['LTDC_GCR_DEPOL_Pos', 'LTDC_GCR_DEPOL_Msk', 'LTDC_GCR_DEPOL',
              'LTDC_CPSR_CXPOS_Pos', 'LTDC_CPSR_CXPOS_Msk', 'LTDC_CPSR_CXPOS',
              'LTDC_CPSR_CYPOS_Pos', 'LTDC_CPSR_CYPOS_Msk', 'LTDC_CPSR_CYPOS']),
    (hal, ['LTDC_DEPOLARITY_AL', 'LTDC_DEPOLARITY_AH']),
    (gpio, ['GPIO_PIN_10'])
]:
    text = path.read_text(encoding='utf-8')
    for name in names:
        m = re.search(r'^#define\s+' + name + r'\s+([^\r\n]+)', text, re.M)
        assert m, name
        definitions.append('#define ' + name + ' ' + m[1].split('/*')[0].strip())

harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef struct { uint32_t GCR, CPSR; } TestLTDC;
typedef struct { uint32_t MODER, AFR[2], IDR; } TestGPIO;
static TestLTDC controller;
static TestGPIO port;
#define LTDC (&controller)
#define GPIOF (&port)
static struct { TestLTDC *Instance; struct { uint32_t DEPolarity; } Init; } hltdc;
#define CLEAR_BIT(reg, bit) ((reg) &= ~(bit))
#define __DSB() ((void)0)
static unsigned scenario, ticks, logs;
static char messages[3][512];

static uint32_t HAL_GetTick(void)
{
  ++ticks;
  bool active = (ticks & 1u) != 0u;
  LTDC->CPSR = (120u << 16u) | (active ? 120u : 16u);
  bool high = active != ((LTDC->GCR & (1u << 29u)) != 0u);
  if (scenario == 1u) LTDC->CPSR = 0u; /* No valid interior scan. */
  if (scenario == 2u) high = false;    /* Stuck-low physical input. */
  if (scenario == 3u) high = (ticks % 3u) != 0u; /* Mixed samples. */
  GPIOF->IDR = high ? (1u << 10u) : 0u;
  if (scenario == 4u) return 0u; /* Frozen SysTick: poll cap must terminate. */
  return ticks;
}

static int DebugLog_Printf(const char *format, ...)
{
  assert(logs < 3u);
  va_list args;
  va_start(args, format);
  int result = vsnprintf(messages[logs++], sizeof messages[0], format, args);
  va_end(args);
  assert(result > 0 && (unsigned)result < sizeof messages[0]);
  return result;
}
'''
harness += '\n'.join(definitions) + '\n' + helper
harness += '\nstatic void ActualOverride(void)\n{\n' + override + '\n}\n'
harness += r'''
static void Initialize(unsigned fault, uint32_t gcr)
{
  scenario = fault; ticks = logs = 0u;
  memset(messages, 0, sizeof messages);
  controller = (TestLTDC){.GCR=gcr};
  port = (TestGPIO){.MODER=2u << 20u, .AFR={0u,14u << 8u}};
  hltdc.Instance = LTDC;
  hltdc.Init.DEPolarity = gcr & LTDC_GCR_DEPOL;
}

int main(void)
{
  assert(LTDC_DEPOLARITY_AL == 0u && LTDC_GCR_DEPOL == 0x20000000u);
  assert(DisplayDE_Region((64u << 16u) | 64u) == 1u);
  assert(DisplayDE_Region((395u << 16u) | 959u) == 1u);
  assert(DisplayDE_Region((64u << 16u) | 10u) == 2u);
  assert(DisplayDE_Region((395u << 16u) | 23u) == 2u);
  const uint32_t outside[] = {(63u << 16u)|100u,(396u << 16u)|100u,
      (100u << 16u)|63u,(100u << 16u)|960u,(100u << 16u)|9u,
      (100u << 16u)|24u};
  for (unsigned i=0u;i<sizeof outside/sizeof outside[0];++i)
    assert(DisplayDE_Region(outside[i]) == 0u);

  Initialize(0u, 0x20002221u);
  ActualOverride();
  assert(logs == 2u && ticks <= 244u);
  assert(strstr(messages[0], "physical=ACTIVE-LOW"));
  assert(strstr(messages[1], "physical=ACTIVE-HIGH"));
  assert(LTDC->GCR == 0x00002221u && hltdc.Init.DEPolarity == 0u);
  puts("PASS actual DE override: active-LOW before / active-HIGH after (modeled pad); cache and GCR synchronized, other bits unchanged");

  Initialize(0u, 0x00012221u); /* Already corrected IOC; preserve DEN/other bits. */
  ActualOverride();
  assert(strstr(messages[0], "physical=ACTIVE-HIGH"));
  assert(strstr(messages[1], "physical=ACTIVE-HIGH"));
  assert(LTDC->GCR == 0x00012221u && hltdc.Init.DEPolarity == 0u);

  for (unsigned fault=1u;fault<=3u;++fault) {
    Initialize(fault, 0x20002221u);
    ActualOverride();
    assert(ticks <= 244u && logs == 2u);
    assert(strstr(messages[1], fault == 1u ? "physical=INSUFFICIENT" : "physical=MIXED"));
    assert(!strstr(messages[1], "physical=ACTIVE-HIGH"));
  }
  Initialize(0u, 0x20002221u);
  GPIOF->AFR[1] = 0u;
  ActualOverride();
  assert(strstr(messages[1], "physical=PIN-CONFIG"));
  Initialize(4u, 0x20002221u);
  ActualOverride();
  assert(logs == 2u && strstr(messages[0], "polls=2000000") &&
         strstr(messages[1], "polls=2000000"));
  puts("PASS DE telemetry: scan interior bounds, idempotent override, insufficient/stuck/mixed/wrong-AF never reported active-HIGH; bounded sampling");
  return 0;
}
'''
test = output / 'test_ltdc_de.c'
test.write_text(harness, encoding='utf-8')
provenance = {str(p.relative_to(firmware)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in [main, device, hal, gpio]}
provenance['scope'] = 'Actual USER blocks and SDK definitions; modeled scan/pad; not physical DE evidence'
(output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(test), '-o', str(output / 'test_ltdc_de')], check=True)
subprocess.run([str(output / 'test_ltdc_de')], check=True)
