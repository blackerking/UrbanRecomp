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
    /* Only the southeast cell has a roof. Its upper-left pixel belongs
     * at (0,0), not (7,7), and uses city CHR even if BG1 has another base. */
    word(rom,0x156a9,0); word(rom,0x156a9+2,0);
    word(rom,0x14f2d+2,1);
    word(ram,0x10200+(120+1)*2,1);
    p->bgTileAdr=4;
    p->vram[16]=0x8000; /* green at roof tile (0,0) */
    assert(ScRendererMapPixel(&r,p,ram,0,0)==0xff00ff00);
    assert(ScRendererMapPixel(&r,p,ram,7,7)==0xff00ff00); /* base green */
    assert(ScRendererMapPixel(&r,p,ram,1,0)==0xff000000); /* transparent roof */
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
    /* A fax desk on BG3 uses a 16-column sheet, not the menu's guessed
     * eight-column repeat. Furniture in lower rows borrows the wood period. */
    memset(p,0,sizeof(*p)); memset(ram,0,0x20000);
    for (int i=0;i<32;++i) p->brightnessMult[i]=(i<<3)|(i>>2);
    p->inidisp=15; p->screenEnabled[0]=4; p->bgXsc[2]=0x50;
    p->cgram[65]=31; p->cgram[66]=31<<5;
    for (int y=0;y<32;++y) for (int x=0;x<32;++x)
        p->vram[0x5000+y*32+x]=0x20+(y%16)*16+x%16;
    for (int row=0;row<8;++row) {
        p->vram[0x20*8+row]=0x00ff;
        p->vram[0x28*8+row]=0xff00;
    }
    ram[0x14]=15;
    assert(ScRendererResize(&r,(ScViewport){512,224,0,0,1}));
    ScRendererLine(&r,p,ram,0,native);
    assert(r.wood_layer==2 && r.wood_period==16);
    assert(r.pixels[256]==0xffff0000 && r.pixels[320]==0xff00ff00);
    memcpy(before,p,sizeof(*p));
    for (int y=1;y<224;++y) ScRendererLine(&r,p,ram,y,native);
    assert(!memcmp(before,p,sizeof(*p)));
    /* $14 advances before the title finishes fading. Keep its scenery until
     * the hardware goes dark; use the current scanline's brightness. */
    memset(p,0,sizeof(*p)); p->inidisp=15; p->bgmode=1;
    p->screenEnabled[0]=1; p->bgXsc[0]=0x40;
    p->brightnessMult[31]=255; p->cgram[1]=31;
    for (int y=0;y<8;++y) p->vram[y]=0xff;
    ram[0x14]=1; ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[256]==0xffff0000);
    ram[0x14]=2; p->inidisp=7; p->brightnessMult[31]=119;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[256]==0xff770000);
    p->inidisp=0x8f; ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[256]==0xff000000 && !r.title_live);
    /* Statistics/tax pages: isolated panel/cursor rows cannot smear their
     * colour across the canvas. The majority already includes the fade. */
    ram[0x14]=0; p->bgmode=0; p->screenEnabled[0]=0; p->inidisp=7;
    for (int y=0;y<224;++y) {
        for (int x=0;x<256;++x) native[x]=(y>=41 && y<=53) ? 0xffbb8844 : 0xff224466;
        ScRendererLine(&r,p,ram,y,native);
    }
    assert(r.pixels[45*512+300]==0xff224466);
    assert(r.pixels[45*512+255]==0xffbb8844);
    /* The selector's offscreen card is real content, not another wood tile. */
    memset(p,0,sizeof(*p)); p->inidisp=15; p->screenEnabled[0]=1;
    p->bgXsc[0]=0x31; p->brightnessMult[31]=255; p->cgram[1]=31;
    p->vram[0x3400]=2;
    for (int y=0;y<8;++y) p->vram[2*8+y]=0xff;
    ram[0x14]=11; ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[256]==0xffff0000);
    assert(r.pixels[480]==0xff000000); /* no repeat of cards beyond strip */
    /* Repeating title lights are OAM, not background tiles. Ignore the
     * parked copy at raw X=257 when finding the visible row's pitch. */
    memset(p,0,sizeof(*p)); p->inidisp=15; p->bgmode=1;
    p->screenEnabled[0]=16; p->brightnessMult[31]=255; p->cgram[129]=31;
    for (int i=0;i<4;++i) p->oam[i*2]=(180<<8)|(1+i*64);
    for (int y=0;y<8;++y) p->vram[y]=0xff;
    p->oam[8]=(180<<8)|1; p->highOam[1]=1;
    ram[0x14]=1; ScRendererLine(&r,p,ram,0,native);
    ScRendererLine(&r,p,ram,179,native);
    assert(r.light_pitch==64 && r.pixels[179*512+257]==0xffff0000);
    /* Fine scroll wraps before WRAM advances its coarse cell. The margin
     * must advance by two pixels, not jump backwards by six. */
    ram[0x14]=0; ram[0x3e]=1; ram[0x1bd]=10; ram[0x1bf]=10;
    p->screenEnabled[0]=2; p->hScroll[1]=86; p->vScroll[1]=80;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==86);
    p->hScroll[1]=88; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==88);
    ram[0x1bd]=11; p->hScroll[1]=90; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==90 && r.scroll_adjust_x==0);
    p->hScroll[1]=88; ScRendererLine(&r,p,ram,0,native);
    p->hScroll[1]=86; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==86);
    ScRendererDestroy(&r); free(p); free(before); free(ram); free(rom);
    puts("PASS: tile flips, overlays, map bounds, native pixels, tall/wide surfaces and PPU immutability");
    return 0;
}
