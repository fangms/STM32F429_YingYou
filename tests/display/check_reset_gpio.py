"""Compile the real MX_GPIO_Init with a GPIO model; this does not measure volts."""
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
sdk = firmware / 'Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_gpio.h'
source = main.read_text(encoding='utf-8')
match = re.search(r'static void MX_GPIO_Init\(void\)\s*\{', source)
assert match
end, depth = match.end(), 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[match.start():end]
header = sdk.read_text(encoding='utf-8')
names = ['GPIO_MODE_Pos', 'GPIO_MODE', 'MODE_INPUT', 'MODE_OUTPUT',
         'OUTPUT_TYPE_Pos', 'OUTPUT_TYPE', 'OUTPUT_PP', 'OUTPUT_OD',
         'GPIO_MODE_INPUT', 'GPIO_MODE_OUTPUT_PP', 'GPIO_MODE_OUTPUT_OD',
         'GPIO_NOPULL', 'GPIO_PULLUP', 'GPIO_PULLDOWN', 'GPIO_SPEED_FREQ_LOW']
definitions = []
for name in names + [f'GPIO_PIN_{i}' for i in range(16)]:
    definition = re.search(r'^#define\s+' + name + r'\s+([^\r\n]+)', header, re.M)
    assert definition, name
    definitions.append('#define ' + name + ' ' + definition[1].split('/*')[0].strip())

stub = '''#ifndef RESET_TEST_HAL_H
#define RESET_TEST_HAL_H
#include <stdint.h>
typedef struct { uint32_t MODER, OTYPER, OSPEEDR, PUPDR, ODR; } GPIO_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
extern GPIO_TypeDef ports[9];
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
extern uint32_t clocks;
'''
for index, port in enumerate('ABCDEFGHI'):
    stub += f'#define GPIO{port} (&ports[{index}])\n'
    stub += f'#define __HAL_RCC_GPIO{port}_CLK_ENABLE() (clocks |= 1u << {index})\n'
stub += '\n'.join(definitions) + '\n#endif\n'
(output / 'stm32f4xx_hal.h').write_text(stub, encoding='utf-8')

harness = r'''
#include "main.h"
#include <assert.h>
#include <stdio.h>
GPIO_TypeDef ports[9];
uint32_t clocks;
static unsigned reset_open_drain_inits;

static void CheckResetDrive(void)
{
  /* A push-pull HIGH would expose the external 1V8 network to MCU 3V3. */
  assert(!(((GPIOD->MODER >> 12u) & 3u) == 1u &&
           (GPIOD->OTYPER & LCD_RST_Pin) == 0u &&
           (GPIOD->ODR & LCD_RST_Pin) != 0u));
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState value)
{
  assert((clocks & (1u << (port - ports))) != 0u);
  if (value == GPIO_PIN_SET) port->ODR |= pin;
  else port->ODR &= ~(uint32_t)pin;
  CheckResetDrive();
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio)
{
  assert((clocks & (1u << (port - ports))) != 0u);
  for (unsigned i = 0u; i < 16u; ++i) {
    uint32_t bit = 1u << i, shift = 2u * i;
    if ((gpio->Pin & bit) == 0u) continue;
    port->OTYPER = (port->OTYPER & ~bit) |
                   (((gpio->Mode & OUTPUT_TYPE) >> OUTPUT_TYPE_Pos) << i);
    port->PUPDR = (port->PUPDR & ~(3u << shift)) | (gpio->Pull << shift);
    port->MODER = (port->MODER & ~(3u << shift)) | ((gpio->Mode & GPIO_MODE) << shift);
  }
  if (port == GPIOD && (gpio->Pin & LCD_RST_Pin) && gpio->Mode == GPIO_MODE_OUTPUT_OD) {
    assert(gpio->Pin == LCD_RST_Pin && gpio->Pull == GPIO_NOPULL);
    assert((GPIOD->ODR & LCD_RST_Pin) == 0u); /* Still held low on mode change. */
    ++reset_open_drain_inits;
  }
  CheckResetDrive();
}
'''
harness += '\n' + function + r'''

int main(void)
{
  assert(LCD_RST_GPIO_Port == GPIOD && LCD_RST_Pin == GPIO_PIN_6);
  for (unsigned boot = 0u; boot < 2u; ++boot) {
    MX_GPIO_Init();
    assert(reset_open_drain_inits == boot + 1u);
    assert(((GPIOD->MODER >> 12u) & 3u) == 1u);
    assert((GPIOD->OTYPER & LCD_RST_Pin) != 0u);
    assert(((GPIOD->PUPDR >> 12u) & 3u) == 0u);
    assert((GPIOD->ODR & LCD_RST_Pin) == 0u);
    /* Only panel RESET changes type; bridge RESET, enables and beep remain PP. */
    assert(GPIOD->OTYPER == LCD_RST_Pin);
    const uint32_t pins = SSD2828_LCD_RESET_Pin | PMU_EN1_Pin | PMU_EN2_Pin |
                          PMU_EN4_Pin | BEEP_Pin;
    for (unsigned i = 0u; i < 16u; ++i)
      if (pins & (1u << i)) assert(((GPIOD->MODER >> (2u * i)) & 3u) == 1u);
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
    assert((GPIOD->ODR & LCD_RST_Pin) && (GPIOD->OTYPER & LCD_RST_Pin));
    /* Leave released for the next initialization, exercising reset ordering. */
  }
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  assert((GPIOD->ODR & LCD_RST_Pin) == 0u && (GPIOD->OTYPER & LCD_RST_Pin));
  puts("PASS actual MX_GPIO_Init: PD6 OD/NOPULL, low before release, no PP HIGH, other PD pins unchanged");
  return 0;
}
'''
test = output / 'test_reset_gpio.c'
test.write_text(harness, encoding='utf-8')
provenance = {str(p.relative_to(firmware)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (main, sdk, firmware / 'Core/Inc/main.h')}
provenance['scope'] = 'Actual MX_GPIO_Init; SDK mode macros; modeled HAL GPIO, no analog voltage or timing'
(output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(output), '-I', str(firmware / 'Core/Inc'),
                str(test), '-o', str(output / 'test_reset_gpio')], check=True)
subprocess.run([str(output / 'test_reset_gpio')], check=True)
