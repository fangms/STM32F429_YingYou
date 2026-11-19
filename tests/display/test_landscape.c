#include "boot_screen.h"
#include "display_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Save(const uint16_t *p, const char *name)
{
  FILE *f = fopen(name, "wb"); assert(f != NULL);
  assert(fputs("P6\n400 960\n255\n", f) >= 0);
  for (size_t i=0u; i<DISPLAY_NATIVE_FRAME_BYTES/2u; ++i) {
    unsigned pixel=p[i];
    fputc(((pixel>>11u)&31u)*255u/31u, f);
    fputc(((pixel>>5u)&63u)*255u/63u, f);
    fputc((pixel&31u)*255u/31u, f);
  }
  assert(fclose(f)==0);
}

int main(void)
{
  const size_t count=DISPLAY_NATIVE_FRAME_BYTES/2u;
  uint16_t *storage=calloc(count+2u,sizeof *storage);
  uint16_t *opposite=calloc(count,sizeof *opposite);
  unsigned char *seen=calloc(count,1u);
  assert(storage && opposite && seen);
  uint16_t *p=storage+1u;
  storage[0]=0x1357u; storage[count+1u]=0x2468u;
  assert(DISPLAY_TEST_STRIDE_BYTES==800u && DISPLAY_NATIVE_FRAME_BYTES==768000u);
  DisplaySurface s;
  assert(!DisplaySurface_Init(NULL,p,DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_ROTATION_CW90));
  assert(!DisplaySurface_Init(&s,NULL,DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_ROTATION_CW90));
  assert(!DisplaySurface_Init(&s,p,DISPLAY_NATIVE_FRAME_BYTES-1u,DISPLAY_ROTATION_CW90));
  assert(!DisplaySurface_Init(&s,(uint16_t *)((char *)p+1u),DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_ROTATION_CW90));
  assert(!DisplaySurface_Init(&s,p,DISPLAY_NATIVE_FRAME_BYTES,(DisplayRotation)180));
  assert(DisplaySurface_Init(&s,p,DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_ROTATION_CW90));
  assert(s.width==960u && s.height==400u);
  assert(DisplaySurface_Index(&s,0u,0u)==399u);
  assert(DisplaySurface_Index(&s,959u,0u)==383999u);
  assert(DisplaySurface_Index(&s,0u,399u)==0u);
  assert(DisplaySurface_Index(&s,959u,399u)==383600u);
  for (unsigned direction=0u;direction<2u;++direction) {
    DisplayRotation rotation=direction ? DISPLAY_ROTATION_CCW90 : DISPLAY_ROTATION_CW90;
    assert(DisplaySurface_Init(&s,p,DISPLAY_NATIVE_FRAME_BYTES,rotation));
    memset(seen,0,count);
    for (unsigned y=0u;y<400u;++y) for (unsigned x=0u;x<960u;++x) {
      size_t index=DisplaySurface_Index(&s,x,y);
      assert(index<count && seen[index]==0u);
      seen[index]=1u;
    }
    for (size_t i=0u;i<count;++i) assert(seen[i]==1u);
    for (unsigned pattern=0u;pattern<DISPLAY_PATTERN_COUNT;++pattern) {
      assert(DisplayTest_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,(DisplayTest_Pattern)pattern,rotation));
      assert(storage[0]==0x1357u && storage[count+1u]==0x2468u);
    }
    assert(BootScreen_DrawDiagnosticOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,rotation));
    unsigned header=BOOT_SCREEN_LANDSCAPE_HEADER_ROWS;
    uint16_t *body=malloc((400u-header)*960u*2u); assert(body);
    for (unsigned y=header;y<400u;++y) for (unsigned x=0u;x<960u;++x)
      body[(y-header)*960u+x]=p[DisplaySurface_Index(&s,x,y)];
    assert(BootScreen_DrawTestLabelOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,8u,
        "LONG TEXT TO EXERCISE RIGHT EDGE CLIPPING AND BODY PRESERVATION",rotation));
    for (unsigned y=header;y<400u;++y) for (unsigned x=0u;x<960u;++x)
      assert(body[(y-header)*960u+x]==p[DisplaySurface_Index(&s,x,y)]);
    free(body);
    assert(BootScreen_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,UINT32_MAX,true,rotation));
    assert(storage[0]==0x1357u && storage[count+1u]==0x2468u);
  }
  assert(!BootScreen_DrawDiagnosticOriented(p,DISPLAY_NATIVE_FRAME_BYTES-1u,true,DISPLAY_ROTATION_CW90));
  assert(!DisplayTest_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_PATTERN_COUNT,DISPLAY_ROTATION_CW90));
  assert(!BootScreen_DrawTestLabelOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,0u,"BAD",DISPLAY_ROTATION_CW90));
  assert(!BootScreen_DrawTestLabelOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,1u,NULL,DISPLAY_ROTATION_CW90));
  /* The two directions must give the same logical image under opposite native
     180-degree ordering. Direction-specific labels deliberately differ. */
  for (unsigned pattern=0u;pattern<DISPLAY_PATTERN_COUNT;++pattern) {
    assert(DisplayTest_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,(DisplayTest_Pattern)pattern,DISPLAY_ROTATION_CW90));
    assert(DisplayTest_DrawOriented(opposite,DISPLAY_NATIVE_FRAME_BYTES,(DisplayTest_Pattern)pattern,DISPLAY_ROTATION_CCW90));
    for (size_t i=0u;i<count;++i) assert(p[i]==opposite[count-1u-i]);
  }
  assert(DisplayTest_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,DISPLAY_PATTERN_GEOMETRY,DISPLAY_ROTATION_CW90));
  /* Independent native corner positions verify CW vs CCW and preserve all edges. */
  assert(p[401u]==0x001Fu && p[798u]==0xF800u);
  assert(p[958u*400u+1u]==0xFFE0u && p[958u*400u+398u]==0x07E0u);
  for (unsigned x=0u;x<400u;++x) assert(p[x]==0xFFFFu && p[959u*400u+x]==0xFFFFu);
  for (unsigned y=0u;y<960u;++y) assert(p[y*400u]==0xFFFFu && p[y*400u+399u]==0xFFFFu);
  Save(p,"landscape_geometry_native.ppm");
  assert(BootScreen_DrawDiagnosticOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,DISPLAY_ROTATION_CW90));
  assert(BootScreen_DrawTestLabelOriented(p,DISPLAY_NATIVE_FRAME_BYTES,true,1u,"FIXED FALLING",DISPLAY_ROTATION_CW90));
  Save(p,"landscape_card_native.ppm");
  assert(BootScreen_DrawOriented(p,DISPLAY_NATIVE_FRAME_BYTES,12u,false,DISPLAY_ROTATION_CW90));
  Save(p,"landscape_boot_native.ppm");
  assert(storage[0]==0x1357u && storage[count+1u]==0x2468u);
  free(storage); free(opposite); free(seen);
  puts("PASS landscape: 960x400 CW90/CCW90 bijection, independent corners/edges, all patterns, labels/clipping/body/guards, invalid arguments and native previews; 400x960 pitch unchanged");
  return 0;
}
