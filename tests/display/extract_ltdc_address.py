"""Compile the actual SDK address/config functions against the host register port.

No firmware SDK file is modified. Generated files and provenance go to the caller's
test output directory. Lock/IRQ/timing behavior is outside this host port's scope.
"""
from pathlib import Path
import hashlib
import json
import re
import sys

fw, target = Path(sys.argv[1]), Path(sys.argv[2])
source = fw / 'Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_ltdc.c'
raw = source.read_bytes()
text = raw.decode('utf-8').replace('\r\n', '\n')


def function(signature):
    found = list(re.finditer(r'^' + re.escape(signature), text, re.M))
    assert len(found) == 1, signature
    start = found[0].start()
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


config = function('static void LTDC_SetConfig(LTDC_HandleTypeDef *hltdc, LTDC_LayerCfgTypeDef *pLayerCfg, uint32_t LayerIdx)\n{')
address = function('HAL_StatusTypeDef HAL_LTDC_SetAddress_NoReload(')
definitions = {}
for header in [fw / 'Drivers/CMSIS/Device/ST/STM32F4xx/Include/stm32f429xx.h',
               fw / 'Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_ltdc.h']:
    definitions.update(re.findall(r'^#define\s+(LTDC_\w+)\s+([^\r\n]+)', header.read_text(encoding='utf-8'), re.M))
required = set(re.findall(r'\bLTDC_[A-Za-z0-9_]+\b', config + address))
required -= {'LTDC_SetConfig','LTDC_LAYER','LTDC_LayerCfgTypeDef','LTDC_HandleTypeDef'}
required &= definitions.keys()  # Ignore prose such as "LTDC_Layer" in SDK comments.
emitted = set()
macros = []


def emit(name):
    if name in emitted:
        return
    expression = definitions[name]
    for dependency in re.findall(r'\bLTDC_\w+\b', expression):
        if dependency != name and dependency in definitions:
            emit(dependency)
    emitted.add(name)
    macros.append(f'#ifndef {name}\n#define {name} {expression}\n#endif\n')


for name in sorted(required):
    emit(name)
preamble = '''
#include "stm32f4xx_hal.h"
#include <assert.h>
#define assert_param(value) assert(value)
#define IS_LTDC_LAYER(layer) ((layer) < 2u)
#define __HAL_LOCK(handle) ((void)(handle))
#define __HAL_UNLOCK(handle) ((void)(handle))
#define WRITE_REG(reg, value) ((reg) = (value))
#define HAL_LTDC_STATE_BUSY 2u
#define HAL_LTDC_STATE_READY 1u
'''
license_header = text[:text.index('*/') + 2]
renamed_address = address.replace('HAL_LTDC_SetAddress_NoReload(', 'TestHAL_LTDC_SetAddress_NoReload(', 1)
target.write_text(license_header + preamble + ''.join(macros) + '\n' + config + '\n\n' + renamed_address + '\n', encoding='utf-8')
provenance = {'source': str(source), 'source_sha256': hashlib.sha256(raw).hexdigest(),
              'functions': {'LTDC_SetConfig': hashlib.sha256(config.encode()).hexdigest(),
                            'HAL_LTDC_SetAddress_NoReload': hashlib.sha256(address.encode()).hexdigest()},
              'only_function_rename': 'HAL_LTDC_SetAddress_NoReload -> TestHAL_LTDC_SetAddress_NoReload',
              'masks_and_pixel_formats_from_sdk_headers': sorted(emitted),
              'output_sha256': hashlib.sha256(target.read_bytes()).hexdigest()}
target.with_suffix('.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
