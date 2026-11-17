#define _GNU_SOURCE
#include "board_sdram.h"
#include "debug_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t tick, hclk, delay_ms, refresh_count, command_count, fail_command;
static FMC_SDRAM_CommandTypeDef commands[512];
FMC_SDRAM_TypeDef test_fmc;
GPIO_TypeDef test_gpioe;
static volatile uint16_t *model_memory;
static size_t model_size;
static uint16_t mask_shadow[4];
static int mask_model, pin_stuck, inject_wide32;
static uint32_t previous_clock, clock_stops, refresh_calls, fail_refresh;
static uint32_t refresh_history[4];
static unsigned mask_writes;
static uint32_t diagnostic_commands;
static void reset_mask_shadow(void)
{
  const size_t offsets[]={0u,1u,model_size/4u,model_size/2u-1u};
  for(unsigned i=0u;i<4u;++i) memcpy(&mask_shadow[i],(const void *)(model_memory+offsets[i]),2u);
}
static void model_barrier(void)
{
  uint32_t clock=test_fmc.SDCR[0]&FMC_SDCR1_SDCLK;
  if(clock!=previous_clock) {
    if(clock==0u) {
      assert(command_count>0u && commands[command_count-1u].CommandMode==FMC_SDRAM_CMD_PALL);
      ++clock_stops;
    } else assert(previous_clock==0u);
    previous_clock=clock;
  }
  if(model_memory!=NULL) {
    const size_t offsets[]={0u,1u,model_size/4u,model_size/2u-1u};
    if(strcmp(DebugLog_GetStage(),"diag-mask-write")==0) {
      assert((test_gpioe.MODER&15u)==5u);
      unsigned changes=0u;
      for(unsigned i=0u;i<4u;++i) {
        uint16_t value;
        memcpy(&value,(const void *)(model_memory+offsets[i]),2u);
        if(value!=mask_shadow[i]) {
          assert(value==0xABCDu && mask_shadow[i]==0x1234u);
          ++changes;
          uint32_t mask=test_gpioe.ODR&3u;
          if(mask_model==2) mask=((mask&1u)<<1u)|((mask&2u)>>1u);
          if(mask_model!=0) {
            uint16_t corrected=(uint16_t)((mask&1u?0x0034u:0x00CDu)|(mask&2u?0x1200u:0xAB00u));
            memcpy((void *)(model_memory+offsets[i]),&corrected,2u);
          }
        }
      }
      assert(changes==1u);
      ++mask_writes;
    }
    reset_mask_shadow();
  }
}
uint32_t test_ipsr, test_primask;
static HAL_StatusTypeDef refresh_status, uart_status;
static char uart_output[131072];
static size_t uart_length;
static volatile uint16_t *fault_memory;
static unsigned probe_barriers;
static int fault_at;
static uint16_t forced_value;
static int fault_requires_fifo;
static const char *fault_phase = "byte-lanes";
void TestDSB(void)
{
  model_barrier();
  if (fault_memory == NULL || strcmp(DebugLog_GetStage(), fault_phase) != 0 ||
      (fault_requires_fifo && (test_fmc.SDCR[0] & FMC_SDCR1_RBURST) == 0u)) return;
  if ((int)probe_barriers++ == fault_at) *fault_memory = forced_value;
}

uint32_t HAL_GetTick(void) { return tick++; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return hclk; }
void HAL_Delay(uint32_t delay) {
  delay_ms += delay; tick += delay;
  if(inject_wide32 && model_memory && strcmp(DebugLog_GetStage(),"diag32-retention-100ms")==0) {
    model_memory[0]^=1u; inject_wide32=0;
  }
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port,uint16_t pins,GPIO_PinState state)
{
  assert(port==GPIOE && (pins&~3u)==0u);
  if(state==GPIO_PIN_SET) port->ODR|=pins; else port->ODR&=~pins;
  port->IDR=(port->IDR&~3u)|(pin_stuck?0u:(port->ODR&3u));
}
void HAL_GPIO_Init(GPIO_TypeDef *port,GPIO_InitTypeDef *cfg)
{
  assert(port==GPIOE && cfg->Pin==3u && cfg->Mode==GPIO_MODE_OUTPUT_PP);
  assert(command_count>0u && commands[command_count-1u].CommandMode==FMC_SDRAM_CMD_PALL);
  port->MODER=(port->MODER&~15u)|5u;
  port->OTYPER&=~3u;port->PUPDR&=~15u;port->OSPEEDR=(port->OSPEEDR&~15u)|15u;
}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, const uint8_t *data,
                                  uint16_t size, uint32_t timeout)
{
  assert(uart != NULL && timeout == 100u);
  if (uart_status != HAL_OK) return uart_status;
  assert(uart_length + size < sizeof uart_output);
  memcpy(uart_output + uart_length, data, size);
  uart_length += size;
  uart_output[uart_length] = '\0';
  return HAL_OK;
}
HAL_StatusTypeDef FMC_SDRAM_SendCommand(FMC_SDRAM_TypeDef *instance,
                                      const FMC_SDRAM_CommandTypeDef *command,
                                      uint32_t timeout)
{
  assert(instance == FMC_SDRAM_DEVICE && timeout == 100u);
  assert(command_count < 512u);
  commands[command_count++] = *command;
  if (command_count > 1u && commands[0].CommandMode == FMC_SDRAM_CMD_CLK_ENABLE)
    assert(delay_ms >= 1u);
  return command_count == fail_command ? HAL_TIMEOUT : HAL_OK;
}
HAL_StatusTypeDef HAL_SDRAM_ProgramRefreshRate(SDRAM_HandleTypeDef *sdram, uint32_t count)
{
  assert(sdram->Instance == FMC_SDRAM_DEVICE);
  assert(refresh_calls<4u);
  refresh_history[refresh_calls++]=count;
  refresh_count = count;
  test_fmc.SDRTR=count<<1u;
  return refresh_calls==fail_refresh?HAL_ERROR:refresh_status;
}

static SDRAM_HandleTypeDef default_sdram(void)
{
  SDRAM_HandleTypeDef result = {0};
  result.Instance = FMC_SDRAM_DEVICE;
  result.State = HAL_SDRAM_STATE_READY;
  result.Init.SDBank = FMC_SDRAM_BANK1;
  result.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_9;
  result.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_13;
  result.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_16;
  result.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
  result.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_3;
  result.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
  return result;
}
static void reset_hal(void)
{
  fault_phase = "byte-lanes"; fault_memory = NULL; probe_barriers = 0u; fault_at = -1; fault_requires_fifo = 0;
  memset(&test_fmc, 0, sizeof test_fmc);
  memset(&test_gpioe,0,sizeof test_gpioe);
  model_memory=NULL;model_size=0u;mask_model=1;pin_stuck=inject_wide32=0;
  previous_clock=clock_stops=refresh_calls=fail_refresh=mask_writes=0u;
  tick = delay_ms = refresh_count = command_count = fail_command = 0u;
  test_ipsr = test_primask = test_fmc.SDSR = 0u;
  hclk = 180000000u;
  refresh_status = uart_status = HAL_OK;
  uart_length = 0u;
  uart_output[0] = '\0';
  DebugLog_Init(NULL);
}

static void test_logger(void)
{
  UART_HandleTypeDef uart = {0};
  char oversized[400];
  reset_hal();
  assert(DebugLog_Write("before-init") == HAL_ERROR);
  DebugLog_Init(&uart);
  assert(DebugLog_Printf("BOOT %u\r\n", 123u) == HAL_OK);
  assert(strcmp(uart_output, "BOOT 123\r\n") == 0);
  test_ipsr = 3u;
  assert(DebugLog_Write("fault") == HAL_BUSY);
  test_ipsr = 0u;
  test_primask = 1u;
  assert(DebugLog_Write("masked") == HAL_BUSY);
  test_primask = 0u;
  assert(DebugLog_Write(NULL) == HAL_ERROR);
  assert(DebugLog_Printf(NULL) == HAL_ERROR);
  DebugLog_SetStage("memory-test");
  assert(strcmp(DebugLog_GetStage(), "memory-test") == 0);
  memset(oversized, 'x', sizeof oversized - 1u);
  oversized[sizeof oversized - 1u] = '\0';
  uart_length = 0u;
  assert(DebugLog_Printf("%s", oversized) == HAL_OK);
  assert(uart_length == DEBUG_LOG_BUFFER_SIZE - 1u);
  assert(__io_putchar(0) == 0);
  assert(uart_length == DEBUG_LOG_BUFFER_SIZE && uart_output[uart_length - 1u] == '\0');
  uart_status = HAL_TIMEOUT;
  assert(DebugLog_Write("timeout") == HAL_TIMEOUT);
  assert(__io_putchar('x') == EOF);
  puts("PASS logger: binding, context guards, formatting bounds, UART timeout");
}

static void test_initialization(void)
{
  BoardSDRAM_Report report;
  SDRAM_HandleTypeDef sdram;
  reset_hal();
  sdram = default_sdram();
  assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_OK);
  assert(command_count == 4u && refresh_count == 683u);
  assert(delay_ms >= 1u && report.sdclock_hz == 90000000u);
  assert(report.mode_register == 0x0230u);
  assert(commands[0].CommandMode == FMC_SDRAM_CMD_CLK_ENABLE);
  assert(commands[1].CommandMode == FMC_SDRAM_CMD_PALL);
  assert(commands[2].CommandMode == FMC_SDRAM_CMD_AUTOREFRESH_MODE);
  assert(commands[2].AutoRefreshNumber == 8u);
  assert(commands[3].CommandMode == FMC_SDRAM_CMD_LOAD_MODE);
  assert(commands[3].ModeRegisterDefinition == 0x0230u);
  for (unsigned i = 0; i < 4u; ++i)
    assert(commands[i].CommandTarget == FMC_SDRAM_CMD_TARGET_BANK1);
  assert(BoardSDRAM_CalculateRefreshCount(90000000u) == 683u);
  assert(BoardSDRAM_CalculateRefreshCount(60000000u) == 448u);
  reset_hal(); sdram=default_sdram();sdram.Init.SDClockPeriod=FMC_SDRAM_CLOCK_PERIOD_3;
  assert(BoardSDRAM_Initialize(&sdram,&report)==BOARD_SDRAM_OK);
  assert(report.sdclock_hz==60000000u && refresh_count==448u);
  assert(report.mode_register==0x0230u);
  sdram.Init.SDClockPeriod=0u;
  assert(BoardSDRAM_Initialize(&sdram,&report)==BOARD_SDRAM_INVALID_CONFIG);
  assert(BoardSDRAM_CalculateRefreshCount(0u) == 0u);
  assert(BoardSDRAM_CalculateRefreshCount(UINT32_MAX) == 0u);
  for (unsigned i = 1u; i <= 4u; ++i) {
    reset_hal(); sdram = default_sdram(); fail_command = i;
    assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_COMMAND_FAILED);
    assert(report.hal_status == HAL_TIMEOUT && command_count == i);
    assert(refresh_count == 0u);
  }
  reset_hal(); sdram = default_sdram(); refresh_status = HAL_ERROR;
  assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_REFRESH_FAILED);
  assert(report.hal_status == HAL_ERROR);
  reset_hal(); sdram = default_sdram(); test_fmc.SDSR = FMC_SDSR_BUSY;
  assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_COMMAND_FAILED);
  assert(command_count == 0u && report.hal_status == HAL_TIMEOUT);
  reset_hal(); sdram = default_sdram(); hclk = 160000000u;
  assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_INVALID_CONFIG);
  assert(command_count == 0u);
  reset_hal(); sdram = default_sdram(); sdram.Init.CASLatency = 2u;
  assert(BoardSDRAM_Initialize(&sdram, &report) == BOARD_SDRAM_INVALID_CONFIG);
  assert(BoardSDRAM_Initialize(NULL, &report) == BOARD_SDRAM_INVALID_CONFIG);
  assert(BoardSDRAM_Initialize(&sdram, NULL) == BOARD_SDRAM_INVALID_CONFIG);
  puts("PASS SDRAM init: command order, 683/0x230, four command timeouts, busy, invalid config");
}

static void test_memory(void)
{
  BoardSDRAM_Report report = {0};
  uint16_t *memory = malloc(BOARD_SDRAM_SIZE_BYTES);
  assert(memory != NULL);
  reset_hal();
  assert(BoardSDRAM_TestMemory(NULL, 4096u, &report) == BOARD_SDRAM_INVALID_BUFFER);
  assert(BoardSDRAM_TestMemory(memory, 3u, &report) == BOARD_SDRAM_INVALID_BUFFER);
  assert(BoardSDRAM_TestMemory(memory, 4096u, NULL) == BOARD_SDRAM_INVALID_BUFFER);
  assert(BoardSDRAM_TestMemory(memory, BOARD_SDRAM_SIZE_BYTES, &report) == BOARD_SDRAM_OK);
  assert(report.tested_bytes == BOARD_SDRAM_SIZE_BYTES && delay_ms >= 100u);
  free(memory);
  puts("PASS SDRAM memory: data/address/byte lanes, full 32MiB patterns, idle retention");

  /* Emulate one missing address line: two virtual pages share the same storage. */
  long page = sysconf(_SC_PAGESIZE);
  int fd = memfd_create("sdram-address-alias", 0);
  assert(page >= 4096 && fd >= 0 && ftruncate(fd, page) == 0);
  void *alias = mmap(NULL, (size_t)page * 2u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(alias != MAP_FAILED);
  assert(mmap(alias, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == alias);
  void *second = (char *)alias + page;
  assert(mmap(second, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == second);
  assert(BoardSDRAM_TestMemory(alias, (size_t)page * 2u, &report) == BOARD_SDRAM_MEMORY_MISMATCH);
  assert(strcmp(report.phase, "address-bus") == 0);
  assert(report.failed_address >= (uintptr_t)alias);
  assert(report.expected != report.actual && report.tested_bytes == 0u);
  munmap(alias, (size_t)page * 2u);
  close(fd);
  puts("PASS SDRAM fault injection: mirrored address detected with mismatch details");
}


static void test_byte_failures(void)
{
  static const struct { int barrier; uint16_t actual, expected; const char *phase; } cases[] = {
    {0, 0x025Au, 0x5A5Au, "byte-seed"},
    {1, 0x02A5u, 0x5AA5u, "byte-low"}, /* Replay the uploaded corruption. */
    {2, 0x3CA4u, 0x3CA5u, "byte-high"},
    {4, 0xA45Au, 0xA55Au, "byte-high"}, /* Reverse byte order. */
    {5, 0xA53Du, 0xA53Cu, "byte-low"}
  };
  for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
    uint16_t memory[32] = {0};
    BoardSDRAM_Report report = {0};
    reset_hal(); fault_memory = memory;
    fault_at = cases[i].barrier; forced_value = cases[i].actual;
    assert(BoardSDRAM_TestMemory(memory, sizeof memory, &report) == BOARD_SDRAM_MEMORY_MISMATCH);
    assert(strcmp(report.phase, cases[i].phase) == 0);
    assert(report.expected == cases[i].expected && report.actual == cases[i].actual);
    assert(report.failed_address == (uintptr_t)memory && report.tested_bytes == 0u);
  }
  {
    uint16_t memory[32] = {0};
    BoardSDRAM_Report report = {0};
    reset_hal(); fault_memory=memory; fault_phase="byte-fast";
    fault_at=1; forced_value=0x02A5u;
    assert(BoardSDRAM_TestMemory(memory,sizeof memory,&report)==BOARD_SDRAM_MEMORY_MISMATCH);
    assert(strcmp(report.phase,"byte-fast-low")==0);
    assert(report.expected==0x5AA5u && report.actual==0x02A5u && report.tested_bytes==0u);
  }
  fault_memory = NULL;
  puts("PASS byte fault injection: seed, low/high preservation, both orders, uploaded 0x02A5 replay");
}

static void prepare_diagnostic(uint16_t *memory,size_t size,SDRAM_HandleTypeDef *sdram,UART_HandleTypeDef *uart)
{
  reset_hal();*sdram=default_sdram();DebugLog_Init(uart);
  test_fmc.SDCR[0]=0x39D9u;test_fmc.SDTR[0]=0x02227471u;test_fmc.SDRTR=683u<<1u;
  previous_clock=FMC_SDRAM_CLOCK_PERIOD_2;
  test_gpioe.MODER=0xAAAA800Au;test_gpioe.AFR[0]=0xC00000CCu;
  test_gpioe.AFR[1]=0x11223344u;test_gpioe.OTYPER=0xBB00u;
  test_gpioe.PUPDR=0x140u;test_gpioe.OSPEEDR=0xFFFFC00Fu;test_gpioe.ODR=0xA5A5u;
  model_memory=memory;model_size=size;reset_mask_shadow();
}
static void assert_gpio_restored(GPIO_TypeDef saved)
{
  assert(test_gpioe.MODER==saved.MODER && test_gpioe.OTYPER==saved.OTYPER);
  assert(test_gpioe.OSPEEDR==saved.OSPEEDR && test_gpioe.PUPDR==saved.PUPDR);
  assert(test_gpioe.AFR[0]==saved.AFR[0] && test_gpioe.AFR[1]==saved.AFR[1]);
  assert(test_gpioe.ODR==saved.ODR);
}
static void test_diagnostic(void)
{
  uint32_t *allocation=calloc(18u,sizeof(uint32_t));
  assert(allocation);uint16_t *memory=(uint16_t *)(allocation+1u);
  allocation[0]=0xFEEDBEEFu;allocation[17]=0x12345678u;
  UART_HandleTypeDef uart={0};SDRAM_HandleTypeDef sdram;
  BoardSDRAM_Report failure={.phase="byte-fast-low",.sdclock_hz=90000000u,
                            .failed_address=(uintptr_t)memory,.expected=0x5AA5u,.actual=0x02A5u};
  BoardSDRAM_Report original=failure;
  prepare_diagnostic(memory,64u,&sdram,&uart);
  GPIO_TypeDef saved=test_gpioe;
  fault_memory=memory;fault_at=1;forced_value=0x02A5u;
  BoardSDRAM_DiagnoseByteAccess(&sdram,memory,64u,&failure);
  assert_gpio_restored(saved);
  assert(mask_writes==32u && clock_stops==2u);
  assert(sdram.Init.SDClockPeriod==FMC_SDRAM_CLOCK_PERIOD_2 && test_fmc.SDCR[0]==0x39D9u);
  assert(test_fmc.SDTR[0]==0x02227471u && test_fmc.SDRTR==(683u<<1u));
  assert(refresh_calls==2u && refresh_history[0]==448u && refresh_history[1]==683u);
  assert(strstr(uart_output,"id=0 clock=90000000Hz refresh=683"));
  assert(strstr(uart_output,"id=1 clock=60000000Hz refresh=448"));
  assert(strstr(uart_output,"failed=0 pin-failed=0 restored=1 control=OK"));
  assert(strstr(uart_output,"[WIDE16] result=PASS bytes=64"));
  assert(strstr(uart_output,"[WIDE32] result=PASS bytes=64"));
  assert(strstr(uart_output,"id=0 native-byte=FAIL"));
  assert(strstr(uart_output,"id=1 native-byte=PASS"));
  assert(strstr(uart_output,"restore-90MHz=OK registers-match=1"));
  assert(strstr(uart_output,"[DIAG2] end; original failure retained; display remains OFF"));
  assert(strcmp(DebugLog_GetStage(),failure.phase)==0 && memcmp(&failure,&original,sizeof failure)==0);
  diagnostic_commands=command_count;
  assert(allocation[0]==0xFEEDBEEFu && allocation[17]==0x12345678u);

  for(int model=0;model<3;++model) {
    prepare_diagnostic(memory,64u,&sdram,&uart);mask_model=model;
    BoardSDRAM_DiagnoseByteAccess(&sdram,memory,64u,&failure);
    assert(strstr(uart_output,model==0?"failed=12 pin-failed=0":model==1?"failed=0 pin-failed=0":"failed=8 pin-failed=0"));
    assert_gpio_restored(saved);assert(mask_writes==32u);
  }
  prepare_diagnostic(memory,64u,&sdram,&uart);pin_stuck=1;inject_wide32=1;
  BoardSDRAM_DiagnoseByteAccess(&sdram,memory,64u,&failure);
  assert(strstr(uart_output,"pin-failed=12"));
  assert(strstr(uart_output,"[WIDE32] result=FAIL bytes=0"));
  assert(strstr(uart_output,"[WIDE32] FAIL addr="));
  assert_gpio_restored(saved);
  free(allocation);
  puts("PASS DIAG2: four masks incl ignored/swapped/stuck models, WIDE16/32, 60MHz/448 and 90MHz/683 restore, byte failure retained, guards");
}
static void test_diagnostic_failures(void)
{
  uint32_t *memory=calloc(16u,sizeof(uint32_t));assert(memory);
  UART_HandleTypeDef uart={0};SDRAM_HandleTypeDef sdram;
  BoardSDRAM_Report failure={.phase="byte-low",.sdclock_hz=90000000u,.expected=0x5AA5u,.actual=0x02A5u};
  BoardSDRAM_Report original=failure;
  for(uint32_t fail=1u;fail<=diagnostic_commands;++fail) {
    prepare_diagnostic((uint16_t *)memory,64u,&sdram,&uart);fail_command=fail;
    GPIO_TypeDef saved=test_gpioe;
    BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,64u,&failure);
    assert(command_count>=fail && memcmp(&failure,&original,sizeof failure)==0);
    assert_gpio_restored(saved);
    assert(strstr(uart_output,"[DIAG2] end; original failure retained"));
    assert(strstr(uart_output,"HAL=3")||strstr(uart_output,"restore-90MHz=FAIL"));
  }
  for(uint32_t fail=1u;fail<=2u;++fail) {
    prepare_diagnostic((uint16_t *)memory,64u,&sdram,&uart);fail_refresh=fail;
    GPIO_TypeDef saved=test_gpioe;
    BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,64u,&failure);
    assert_gpio_restored(saved);assert(memcmp(&failure,&original,sizeof failure)==0);
    assert(strstr(uart_output,fail==1u?"clock-change FAIL":"restore-90MHz=FAIL"));
  }
  prepare_diagnostic((uint16_t *)memory,64u,&sdram,&uart);test_fmc.SDSR=FMC_SDSR_BUSY;
  BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,64u,&failure);
  assert(command_count==0u && strstr(uart_output,"mask-enter PALL HAL=3"));
  assert(mask_writes==0u);
  reset_hal();sdram=default_sdram();
  BoardSDRAM_DiagnoseByteAccess(NULL,(uint16_t *)memory,64u,&failure);
  BoardSDRAM_DiagnoseByteAccess(&sdram,NULL,64u,&failure);
  BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,63u,&failure);
  BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,64u,NULL);
  BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory+1u,64u,&failure);
  failure.phase="address-bus";
  BoardSDRAM_DiagnoseByteAccess(&sdram,(uint16_t *)memory,64u,&failure);
  assert(command_count==0u);
  free(memory);
  printf("PASS DIAG2 failures: %u command-site timeouts, both refresh failures, BUSY, invalid input, GPIO restoration\n",diagnostic_commands);
}
static void test_full_capacity_diagnostic(void)
{
  uint16_t *memory=malloc(BOARD_SDRAM_SIZE_BYTES);assert(memory);
  UART_HandleTypeDef uart={0};SDRAM_HandleTypeDef sdram;
  BoardSDRAM_Report failure={.phase="byte-low",.sdclock_hz=90000000u,.actual=0x02A5u};
  prepare_diagnostic(memory,BOARD_SDRAM_SIZE_BYTES,&sdram,&uart);
  BoardSDRAM_DiagnoseByteAccess(&sdram,memory,BOARD_SDRAM_SIZE_BYTES,&failure);
  assert(strstr(uart_output,"[WIDE16] result=PASS bytes=33554432"));
  assert(strstr(uart_output,"[WIDE32] result=PASS bytes=33554432"));
  assert(strstr(uart_output,"id=1 native-byte=PASS"));
  assert(failure.actual==0x02A5u && failure.tested_bytes==0u);
  free(memory);
  puts("PASS DIAG2 full 32MiB: 16/32-bit patterns, unique addresses, retention at both profiles; initial failure preserved");
}

int main(void)
{
  test_logger();
  test_initialization();
  test_memory();
  test_byte_failures();
  test_diagnostic();
  test_diagnostic_failures();
  test_full_capacity_diagnostic();
  puts("ALL TESTS PASSED");
  return 0;
}
