#include "sc_renderer.h"
#include "snes/ppu.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void word(uint8_t *data,unsigned at,unsigned value) { data[at]=value; data[at+1]=value>>8; }
int main(void) {
    Ppu *p=calloc(1,sizeof(*p)), *before=malloc(sizeof(*p));
    uint8_t *ram=calloc(1,0x20000), *rom=calloc(1,0x80000);
    assert(p && before && ram && rom);
    for (int i=0;i<32;++i) p->brightnessMult[i]=(i<<3)|(i>>2);
    p->inidisp=15; p->bgmode=1; p->screenEnabled[0]=2;
    p->cgram[1]=31; p->cgram[2]=31<<5;
    /* One red pixel at tile (0,0), one green at (7,7). */
    p->vram[0]=0x0080; p->vram[7]=0x0100;
    for (int i=0;i<958;++i) word(rom,0x14f2d+i*2,0x300);
    ScRenderer r; ScRendererInit(&r,rom,0x80000,true);
    assert(ScRendererMapPixel(&r,p,ram,0,0)==0xffff0000);
    assert(ScRendererMapPixel(&r,p,ram,7,7)==0xff00ff00);
    word(rom,0x156a9,0xc000);
    assert(ScRendererMapPixel(&r,p,ram,7,7)==0xffff0000);
    assert(ScRendererMapPixel(&r,p,ram,0,0)==0xff00ff00);
    assert(ScRendererMapPixel(&r,p,ram,-1,0)==0xff000000);
    assert(ScRendererMapPixel(&r,p,ram,960,800)==0xff000000);
    word(ram,0x10200,1023);
    assert(ScRendererMapPixel(&r,p,ram,0,0)==0xff000000);
    word(ram,0x10200,0);
    /* Transparent overlay pixel from adjacent cell lands at -1,-1. */
    word(rom,0x14f2d,0);
    assert(ScRendererMapPixel(&r,p,ram,7,7)==0xffff0000);
    ram[0x3e]=1;
    ScVideoSettings settings={true,SC_FIT,true};
    ScViewport v=ScVideoViewport(&settings,720,1280);
    assert(ScRendererResize(&r,v));
    uint32_t native[256];
    for (int x=0;x<256;++x) native[x]=(unsigned)x<<8;
    memcpy(before,p,sizeof(*p));
    for (int y=0;y<224;++y) ScRendererLine(&r,p,ram,y,native);
    assert(!memcmp(before,p,sizeof(*p))); /* renderer never mutates PPU */
    for (int y=0;y<224;++y)
        assert(!memcmp(r.pixels+(size_t)(y+v.core_y)*v.width+v.core_x,native,sizeof(native)));
    settings.aspect=SC_32_9; v=ScVideoViewport(&settings,3840,1080);
    assert(ScRendererResize(&r,v));
    for (int y=0;y<224;++y) ScRendererLine(&r,p,ram,y,native);
    assert(!memcmp(before,p,sizeof(*p)));
    assert(!ScRendererResize(&r,(ScViewport){8192,224,0,0,1}));
    ScRendererDestroy(&r); free(p); free(before); free(ram); free(rom);
    puts("PASS: tile flips, overlays, map bounds, native pixels, tall/wide surfaces and PPU immutability");
    return 0;
}
