#include "board_test.h"
#include "boot_screen.h"
#include "debug_log.h"
#include "main.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GPIO_TypeDef test_gpio[9];
TIM_TypeDef test_tim7;
RCC_TypeDef test_rcc;
uint32_t test_primask, test_ipsr;
static uint32_t inputs[9], now, hclk, pclk, edges, tone_highs;
static bool irq_enabled, address_fail;
static char logs[65536];
static size_t log_size;
static const unsigned ports[]={6u,6u,6u,4u,4u};
static const uint16_t pins[]={ON_OFF_KEY_Pin,SET_KEY_Pin,UP_KEY_Pin,DN_KEY_Pin,OK_KEY_Pin};

uint32_t HAL_GetTick(void) { return now; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return hclk; }
uint32_t HAL_RCC_GetPCLK1Freq(void) { return pclk; }
void TestTIM7Reset(void) { memset(&test_tim7,0,sizeof test_tim7); }
void HAL_NVIC_DisableIRQ(int irq) { assert(irq==TIM7_IRQn); irq_enabled=false; }
void HAL_NVIC_EnableIRQ(int irq) { assert(irq==TIM7_IRQn); irq_enabled=true; }
void HAL_NVIC_ClearPendingIRQ(int irq) { assert(irq==TIM7_IRQn); }
void HAL_NVIC_SetPriority(int irq,uint32_t pre,uint32_t sub)
{ assert(irq==TIM7_IRQn && pre==6u && sub==0u); }

void HAL_GPIO_WritePin(GPIO_TypeDef *port,uint16_t pin,GPIO_PinState state)
{
  assert(port==BEEP_GPIO_Port && pin==BEEP_Pin); /* No pump/valve/power writes. */
  bool old=(port->ODR&pin)!=0u;
  if (state==GPIO_PIN_SET) port->ODR|=pin; else port->ODR&=~pin;
  if (old!=(state==GPIO_PIN_SET)) ++edges;
  if (state==GPIO_PIN_SET) ++tone_highs;
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port,uint16_t pin)
{ return (inputs[port-test_gpio]&pin)!=0u ? GPIO_PIN_SET : GPIO_PIN_RESET; }
HAL_StatusTypeDef HAL_LTDC_SetAddress_NoReload(LTDC_HandleTypeDef *h,uint32_t address,uint32_t layer)
{
  assert(layer==0u && (h->Instance->SRCR&LTDC_SRCR_VBR)==0u);
  if (address_fail) return HAL_ERROR;
  h->LayerCfg[0].FBStartAdress=address;
  h->Instance->layers[0].CFBAR=address;
  return HAL_OK;
}
HAL_StatusTypeDef DebugLog_Write(const char *text)
{
  size_t size=strlen(text);
  assert(log_size+size<sizeof logs);
  memcpy(logs+log_size,text,size+1u); log_size+=size;
  return HAL_OK;
}
HAL_StatusTypeDef DebugLog_Printf(const char *format,...)
{
  char text[DEBUG_LOG_BUFFER_SIZE]; va_list args; va_start(args,format);
  int size=vsnprintf(text,sizeof text,format,args); va_end(args);
  assert(size>=0 && (size_t)size<sizeof text);
  return DebugLog_Write(text);
}

static void Init(BoardTest *s,uint32_t start)
{
  memset(test_gpio,0,sizeof test_gpio);
  for (unsigned i=0u;i<9u;++i) inputs[i]=0xFFFFu;
  test_gpio[1].ODR=FLOW_EN_Pin|SPI2_CS_Pin;
  test_gpio[2].ODR=SPI_NSS_Pin;
  test_rcc=(RCC_TypeDef){.CFGR=5u<<RCC_CFGR_PPRE1_Pos};
  hclk=180000000u; pclk=45000000u; now=start;
  test_primask=test_ipsr=edges=tone_highs=0u;
  log_size=0u; logs[0]='\0'; address_fail=false;
  BoardTest_Init(s,start);
  assert(irq_enabled && TIM7->PSC==89u && TIM7->ARR==249u);
  assert(BoardTest_GetBuzzer().ready && !BoardTest_GetBuzzer().active);
  assert((TIM7->CR1&TIM_CR1_CEN)==0u);
}
static void Key(unsigned key,bool down)
{
  if (down) inputs[ports[key]]&=~pins[key]; else inputs[ports[key]]|=pins[key];
}
static void Update(BoardTest *s,uint32_t tick) { now=tick; BoardTest_Update(s,tick); }
static void Irqs(unsigned count)
{
  for (unsigned i=0u;i<count;++i) if ((TIM7->CR1&TIM_CR1_CEN)!=0u) {
    assert(irq_enabled && test_primask==0u);
    TIM7->SR=TIM_SR_UIF; TIM7_IRQHandler();
  }
}
static unsigned Occurrences(const char *term)
{
  unsigned n=0u;
  for (const char *p=logs;(p=strstr(p,term))!=NULL;p+=strlen(term)) ++n;
  return n;
}
static void Press(BoardTest *s,unsigned key,uint32_t tick)
{ Key(key,true); Update(s,tick); Update(s,tick+30u); }
static void Release(BoardTest *s,unsigned key,uint32_t tick)
{ Key(key,false); Update(s,tick); Update(s,tick+30u); }

static void KeysAndSound(void)
{
  BoardTest s; Init(&s,0u);
  Key(2u,true); Update(&s,5u);
  Key(2u,false); Update(&s,10u);
  Key(2u,true); Update(&s,15u); Update(&s,44u);
  assert(s.keys[2].presses==0u);
  Update(&s,45u);
  assert(s.keys[2].down && s.keys[2].presses==1u && s.last_key==2u);
  assert(Occurrences("name=UP state=DOWN")==1u);
  BoardTest_Buzzer b=BoardTest_GetBuzzer();
  assert(b.requested==1u && b.started==1u && b.dropped==0u && b.active);
  Irqs(319u); assert((TIM7->CR1&TIM_CR1_CEN)!=0u);
  Irqs(1u); assert(edges==320u && tone_highs==160u && (GPIOD->ODR&BEEP_Pin)==0u);
  Irqs(120u); assert(!BoardTest_GetBuzzer().active && (TIM7->CR1&TIM_CR1_CEN)==0u);
  /* A stalled main loop cannot leave the buzzer energized. */
  for (uint32_t tick=100u;tick<=3000u;tick+=10u) Update(&s,tick);
  assert(s.keys[2].presses==1u && BoardTest_GetBuzzer().requested==1u);
  Key(2u,false); Update(&s,3010u);
  Key(2u,true); Update(&s,3020u);
  Key(2u,false); Update(&s,3025u); Update(&s,3055u);
  assert(!s.keys[2].down && Occurrences("name=UP state=UP")==1u);
  Press(&s,2u,3060u); assert(s.keys[2].presses==2u);
  Irqs(440u); Release(&s,2u,3100u);
  for (unsigned i=0u;i<5u;++i) Key(i,true);
  Update(&s,3200u); Update(&s,3230u);
  b=BoardTest_GetBuzzer(); assert(b.requested==7u && b.started==3u && b.pending==4u);
  Irqs(5u*440u);
  b=BoardTest_GetBuzzer(); assert(b.started==7u && !b.active && b.dropped==0u);
  for (unsigned i=0u;i<5u;++i) assert(s.keys[i].down && s.keys[i].presses==(i==2u?3u:1u));
  assert(GPIOB->ODR==(FLOW_EN_Pin|SPI2_CS_Pin) && GPIOC->ODR==SPI_NSS_Pin);
  assert(test_primask==0u);
  puts("PASS key/sound: bounce, long hold, release/repress, five simultaneous keys; 320 transitions/160 cycles per 80ms at 2kHz; 30ms gaps; no main-loop dependency or unrelated outputs");
}

static void QueueAndWrap(void)
{
  BoardTest s; Init(&s,UINT32_MAX-20u);
  Key(0u,true); Update(&s,UINT32_MAX-10u); Update(&s,19u);
  assert(s.keys[0].down && s.keys[0].presses==1u);
  Init(&s,0u); Key(3u,true); BoardTest_Init(&s,0u);
  Update(&s,29u); assert(s.keys[3].presses==0u);
  Update(&s,30u); assert(s.keys[3].presses==1u); /* Held at initialization. */
  Init(&s,0u);
  for (unsigned i=0u;i<40u;++i) {
    Press(&s,0u,i*80u);
    Release(&s,0u,i*80u+40u);
  }
  BoardTest_Buzzer b=BoardTest_GetBuzzer();
  assert(b.requested==40u && b.started==1u && b.pending==32u && b.dropped==7u);
  assert(s.keys[0].presses==40u && Occurrences("state=DOWN")==40u);
  Irqs(33u*440u); b=BoardTest_GetBuzzer();
  assert(b.started==33u && b.pending==0u && !b.active && (GPIOD->ODR&BEEP_Pin)==0u);
  test_primask=1u; (void)BoardTest_GetBuzzer(); assert(test_primask==1u); test_primask=0u;
  /* Check TIMPRE=1 and APB=1 or 8 as well as the real APB=4 path. */
  test_rcc.DCKCFGR=RCC_DCKCFGR_TIMPRE;
  BoardTest_Init(&s,0u); assert(TIM7->PSC==179u);
  test_rcc.CFGR=6u<<RCC_CFGR_PPRE1_Pos; pclk=22500000u;
  BoardTest_Init(&s,0u); assert(TIM7->PSC==89u);
  test_rcc.CFGR=0u; test_rcc.DCKCFGR=0u; pclk=180000000u;
  BoardTest_Init(&s,0u); assert(TIM7->PSC==179u);
  pclk=12345u; BoardTest_Init(&s,0u);
  assert(!BoardTest_GetBuzzer().ready && !irq_enabled);
  Press(&s,0u,10u); assert(s.keys[0].presses==1u && BoardTest_GetBuzzer().dropped==1u);
  puts("PASS queue/time: bounded queue + explicit drop counter; IRQ mask preserved; UINT32 tick wrap; TIMPRE/APB timer clocks; unsupported clock keeps key logging alive");
}

static uint32_t Hash(const uint16_t *p)
{
  uint32_t h=0u;
  for (size_t i=0u;i<DISPLAY_NATIVE_FRAME_BYTES/2u;++i) h=(h*31u)^p[i];
  return h;
}
static void Save(const uint16_t *p,const char *name)
{
  FILE *f=fopen(name,"wb"); assert(f!=NULL);
  assert(fputs("P6\n400 960\n255\n",f)>=0);
  for (size_t i=0u;i<DISPLAY_NATIVE_FRAME_BYTES/2u;++i) {
    unsigned v=p[i]; fputc((v>>11u)*255u/31u,f);
    fputc(((v>>5u)&63u)*255u/63u,f); fputc((v&31u)*255u/31u,f);
  }
  assert(fclose(f)==0);
}
static void Display(void)
{
  const size_t count=DISPLAY_NATIVE_FRAME_BYTES/2u;
  uint16_t *memory=calloc(2u*count+4u,sizeof *memory); assert(memory!=NULL);
  uint16_t *f0=memory+1u,*f1=memory+count+3u;
  memory[0]=0x1234u; memory[count+1u]=0x5678u;
  memory[count+2u]=0xABCDu; memory[2u*count+3u]=0xDEADu;
  BoardTest s; Init(&s,0u);
  BoardTest_SetStartup(&s,true,true,0x2828u,true);
  LTDC_TypeDef r={.GCR=LTDC_GCR_LTDCEN}; LTDC_HandleTypeDef h={.Instance=&r};
  assert(!BoardTest_AttachDisplay(&s,&h,f0,f0,DISPLAY_NATIVE_FRAME_BYTES));
  assert(!BoardTest_AttachDisplay(&s,&h,f0,f1,DISPLAY_NATIVE_FRAME_BYTES-1u));
  assert(!BoardTest_DrawPage(NULL,f0,DISPLAY_NATIVE_FRAME_BYTES,0u));
  assert(!BootScreen_DrawText(f0,DISPLAY_NATIVE_FRAME_BYTES,0u,0u,"UP",0u,0u,DISPLAY_ROTATION_CW90));
  assert(!BootScreen_DrawText(f0,DISPLAY_NATIVE_FRAME_BYTES,UINT32_MAX,0u,"UP",2u,0u,DISPLAY_ROTATION_CW90));
  assert(BoardTest_AttachDisplay(&s,&h,f0,f1,DISPLAY_NATIVE_FRAME_BYTES));
  assert(s.pending && s.front==1u); uint32_t initial=Hash(f0);
  Save(f0,"key_idle_native.ppm");
  Update(&s,20u); assert(Hash(f0)==initial); /* Pending reload: no writes. */
  r.SRCR=0u; Update(&s,21u); assert(s.front==0u && !s.pending);
  Press(&s,2u,25u); assert(s.pending && s.front==0u && Hash(f0)==initial);
  Save(f1,"key_up_pressed_native.ppm");
  uint32_t changed=Hash(f1); assert(changed!=initial);
  Release(&s,2u,60u);
  assert(Hash(f0)==initial && Hash(f1)==changed); /* Neither possible owner is touched. */
  r.SRCR=0u; Update(&s,95u); assert(s.front==1u && s.pending);
  assert(Hash(f1)==changed);
  r.SRCR=0u; Update(&s,100u);
  for (unsigned i=0u;i<5u;++i) Key(i,true);
  Update(&s,110u); Update(&s,140u); r.SRCR=0u; Update(&s,150u);
  assert(BoardTest_DrawPage(&s,f0,DISPLAY_NATIVE_FRAME_BYTES,123000u));
  Save(f0,"key_all_pressed_native.ppm");
  for (unsigned i=0u;i<5u;++i) s.keys[i].presses=UINT32_MAX;
  assert(BoardTest_DrawPage(&s,f0,DISPLAY_NATIVE_FRAME_BYTES,UINT32_MAX));
  assert(BootScreen_DrawText(f0,DISPLAY_NATIVE_FRAME_BYTES,950u,395u,"TEST",32u,0xFFFFu,DISPLAY_ROTATION_CW90));
  assert(memory[0]==0x1234u && memory[count+1u]==0x5678u && memory[count+2u]==0xABCDu && memory[2u*count+3u]==0xDEADu);
  /* Timeout fails display without preventing keys and sound. */
  Init(&s,200u); BoardTest_SetStartup(&s,true,true,0x2828u,true);
  r=(LTDC_TypeDef){.GCR=LTDC_GCR_LTDCEN};
  assert(BoardTest_AttachDisplay(&s,&h,f0,f1,DISPLAY_NATIVE_FRAME_BYTES));
  Update(&s,300u); assert(s.display_failed && !s.attached && s.display_errors==1u && (r.GCR&1u)==0u);
  Press(&s,4u,310u); assert(s.keys[4].presses==1u && BoardTest_GetBuzzer().requested==1u);
  Init(&s,0u); r=(LTDC_TypeDef){0}; address_fail=true;
  assert(!BoardTest_AttachDisplay(&s,&h,f0,f1,DISPLAY_NATIVE_FRAME_BYTES) && s.display_failed);
  Init(&s,0u); r=(LTDC_TypeDef){.GCR=LTDC_GCR_LTDCEN};
  assert(BoardTest_AttachDisplay(&s,&h,f0,f1,DISPLAY_NATIVE_FRAME_BYTES));
  r.SRCR=0u; Update(&s,1u); r.ISR=LTDC_ISR_TERRIF; Update(&s,2u);
  assert(s.display_failed && s.display_errors==1u && (r.GCR&1u)==0u);
  Press(&s,1u,10u); assert(s.keys[1].presses==1u && BoardTest_GetBuzzer().requested==1u);
  free(memory);
  puts("PASS display: CW90 real renderer + guards/previews, double buffer active/pending ownership, invalid arguments, VBlank timeout/address failure, continued key/sound without display");
}

int main(void)
{
  KeysAndSound(); QueueAndWrap(); Display();
  puts("PASS board_test (ASan + UBSan); physical buttons, tone and display still require board validation");
  return 0;
}
