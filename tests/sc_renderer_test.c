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
    assert(r.view.core_x==128);
    assert(r.pixels[384]==0xffff0000 && r.pixels[448]==0xff00ff00);
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
    assert(r.view.core_x==128 && r.pixels[384]==0xffff0000);
    ram[0x14]=2; p->inidisp=7; p->brightnessMult[31]=119;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[384]==0xff770000);
    p->inidisp=0x8f; ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[384]==0xff000000 && !r.title_live);
    /* Statistics/tax pages: isolated panel/cursor rows cannot smear their
     * colour across the canvas. The majority already includes the fade. */
    ram[0x14]=0; p->bgmode=0; p->screenEnabled[0]=0; p->inidisp=7;
    for (int y=0;y<224;++y) {
        for (int x=0;x<256;++x) native[x]=(y>=41 && y<=53) ? 0xffbb8844 : 0xff224466;
        ScRendererLine(&r,p,ram,y,native);
    }
    assert(r.pixels[45*512+450]==0xff224466);
    assert(r.pixels[45*512+383]==0xffbb8844);
    /* The selector's offscreen card is real content, not another wood tile. */
    memset(p,0,sizeof(*p)); p->inidisp=15; p->screenEnabled[0]=1;
    p->bgXsc[0]=0x31; p->brightnessMult[31]=255; p->cgram[1]=31;
    p->vram[0x3400]=2;
    for (int y=0;y<8;++y) p->vram[2*8+y]=0xff;
    assert(ScRendererResize(&r,(ScViewport){800,224,0,0,1}));
    ram[0x14]=11; ScRendererLine(&r,p,ram,0,native);
    assert(r.pixels[272+256]==0xffff0000);
    assert(r.pixels[272+480]==0xff000000); /* no repeat of cards beyond strip */
    /* Repeating title lights are OAM, not background tiles. Ignore the
     * parked copy at raw X=257 when finding the visible row's pitch. */
    memset(p,0,sizeof(*p)); p->inidisp=15; p->bgmode=1;
    assert(ScRendererResize(&r,(ScViewport){512,224,0,0,1}));
    p->screenEnabled[0]=16; p->brightnessMult[31]=255; p->cgram[129]=31;
    for (int i=0;i<4;++i) p->oam[i*2]=(180<<8)|(1+i*64);
    for (int y=0;y<8;++y) p->vram[y]=0xff;
    p->oam[8]=(180<<8)|1; p->highOam[1]=1;
    ram[0x14]=1; ScRendererLine(&r,p,ram,0,native);
    ScRendererLine(&r,p,ram,179,native);
    assert(r.light_pitch==64 && r.pixels[179*512+385]==0xffff0000);
    /* Fine scroll wraps before WRAM advances its coarse cell. The margin
     * must advance by two pixels, not jump backwards by six. */
    ram[0x14]=0; ram[0x3e]=1; ram[0x1bd]=10; ram[0x1bf]=10;
    p->screenEnabled[0]=2; p->hScroll[1]=86; p->vScroll[1]=80;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.view.core_x==0 && r.scroll_x+r.scroll_adjust_x==86);
    p->hScroll[1]=88; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==88);
    ram[0x1bd]=11; p->hScroll[1]=90; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==90 && r.scroll_adjust_x==0);
    p->hScroll[1]=88; ScRendererLine(&r,p,ram,0,native);
    p->hScroll[1]=86; ScRendererLine(&r,p,ram,0,native);
    assert(r.scroll_x+r.scroll_adjust_x==86);
    /* A moving vehicle keeps its positive X across 255 and the classic
     * 352-pixel limit. A teleported parked HUD slot must remain hidden. */
    p->screenEnabled[0]=18; p->oam[0]=(100<<8)|252; p->oam[1]=0x3800;
    p->cgram[193]=31; p->highOam[0]=0;
    ScRendererLine(&r,p,ram,0,native);
    for (int x=256;x<=360;x+=4) {
        p->oam[0]=(100<<8)|(x&255); p->highOam[0]=1;
        ScRendererLine(&r,p,ram,0,native);
        ScRendererLine(&r,p,ram,99,native);
        assert(r.pixels[99*512+x]==0xffff0000);
    }
    p->cgram[1]=31<<5; /* opaque terrain */
    for (int y=0;y<8;++y) p->vram[16+y]=0xff;
    p->oam[1]=0x0801; /* low-priority red object behind terrain */
    ScRendererLine(&r,p,ram,99,native);
    assert(r.pixels[99*512+360]==0xff00ff00);
    p->oam[1]=0x3801;
    ScRendererLine(&r,p,ram,99,native);
    assert(r.pixels[99*512+360]==0xffff0000);
    p->oam[0]=(100<<8)|128; p->highOam[0]=1;
    ScRendererLine(&r,p,ram,0,native);
    assert(!r.object_grace[0]);
    /* In-game load writes a new map before the old city's fade ends. Keep
     * the prior map and palette until dark -> lit, then release together. */
    ScRendererLine(&r,p,ram,0,native);
    unsigned old_palette=r.held_ppu->cgram[1];
    for (int i=0;i<5000;++i) word(ram,0x10200+i*2,7);
    p->cgram[1]=123; ram[0x1bd]=20;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.map_hold && r.map_confirmed && r.held_ppu->cgram[1]==old_palette);
    p->inidisp=0x8f; ScRendererLine(&r,p,ram,0,native);
    assert(r.map_hold && r.map_dark && r.pixels[256]==0xff000000);
    p->inidisp=15; ScRendererLine(&r,p,ram,0,native);
    assert(!r.map_hold && r.held_ppu->cgram[1]==123);
    /* Wrapped guest edge tiles are replaced by world terrain; an opaque HUD
     * tile protects the whole edge band on its row. The interior stays exact. */
    memset(p,0,sizeof(*p)); memset(ram,0,0x20000);
    p->inidisp=15; p->bgmode=1; p->screenEnabled[0]=2;
    p->brightnessMult[31]=255; p->cgram[1]=31<<5;
    ram[0x3e]=1; ram[0x1bd]=10; ram[0x1bf]=10;
    for (int y=0;y<8;++y) p->vram[y]=0xff;
    for (int x=0;x<256;++x) native[x]=0xffff0000;
    ScRendererResetHistory(&r); ScRendererLine(&r,p,ram,0,native);
    assert(r.repaired_edges[0]==3);
    assert(r.pixels[0]==0xff00ff00 && r.pixels[255]==0xff00ff00);
    assert(r.pixels[8]==0xffff0000 && r.pixels[247]==0xffff0000);
    p->screenEnabled[0]=6; p->bgTileAdr=0x100; p->bgXsc[2]=0x60;
    p->vram[0x6000]=1;
    for (int y=0;y<8;++y) p->vram[0x1008+y]=0xff;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.repaired_edges[0]==2 && r.pixels[0]==0xffff0000);
    /* Center only the advisor's opaque BG3/OBJ pixels. The dimmed BG1 HUD
     * stays left, transparent page pixels reveal the stationary city, and
     * even black OBJ pixels remain opaque. Exercise both axes at once. */
    memset(p,0,sizeof(*p)); memset(ram,0,0x20000);
    p->inidisp=15; p->bgmode=1; p->screenEnabled[0]=20; p->screenEnabled[1]=3;
    p->cgwsel=2; p->cgadsub=0x60; p->bgTileAdr=0x321;
    /* Both city layers were hidden under the original page. Moving that
     * page must reveal the background, without moving/mutating the window. */
    p->screenWindowed[1]=3; p->windowsel=0x22;
    p->window1left=24; p->window1right=247;
    p->bgXsc[0]=0x40; p->bgXsc[1]=0x48; p->bgXsc[2]=0x50;
    p->cgram[1]=31<<5; p->cgram[2]=31; p->cgram[3]=31<<10;
    for (int i=0;i<32;++i) p->brightnessMult[i]=(i<<3)|(i>>2);
    for (int y=0;y<8;++y) {
        p->vram[0x2000+y]=0xff;
        p->vram[0x1010+y]=0xff00;
        p->vram[0x3008+y]=0xdfdf; /* transparent black glyph at x=42 */
    }
    for (int y=0;y<32;++y) {
        p->vram[0x4000+y*32+2]=1;
        p->vram[0x5000+y*32+5]=1;
    }
    p->objBuffer.data[kPpuExtraLeftRight+64]=0x2080;
    ram[0x3e]=1; ram[0x1bd]=10; ram[0x1bf]=10;
    for (int x=0;x<256;++x) native[x]=x==42 ? 0 : (x>=40 && x<48) ? 0x000000ff :
        x==64 ? 0 : (x>=16 && x<24) ? 0x007b0000 : 0x00007b00;
    ScRendererResetHistory(&r);
    assert(ScRendererResize(&r,(ScViewport){684,448,0,0,1}));
    memcpy(before,p,sizeof(*p));
    for (int y=0;y<224;++y) ScRendererLine(&r,p,ram,y,native);
    assert(r.advisor_frame && r.view.core_x==0 && r.view.core_y==0);
    assert(!memcmp(before,p,sizeof(*p)));
    assert(r.pixels[20*684+16]==0xff7b0000); /* HUD did not move */
    assert(r.pixels[20*684+40]==0xff007b00); /* no old page */
    assert(r.pixels[(112+20)*684+214+40]==0xff0000ff);
    assert(r.pixels[(112+20)*684+214+42]==0xff000000); /* black page lettering */
    assert(r.pixels[(112+20)*684+214+64]==0xff000000);
    assert(!r.advisor_pixels[20*256+50]); /* transparent page background */
    assert(r.pixels[(112+20)*684+214+50]==0xff007b00);
    p->inidisp=0x8f;
    for (int x=0;x<256;++x) native[x]=0;
    for (int y=0;y<224;++y) ScRendererLine(&r,p,ram,y,native);
    assert(r.pixels[(112+20)*684+214+40]==0xff000000);
    p->inidisp=15; p->screenEnabled[0]=23; p->screenEnabled[1]=4;
    ScRendererLine(&r,p,ram,0,native);
    assert(!r.advisor_frame && r.view.core_x==0); /* returning to the city */
    ram[0x14]=1;
    ScRendererLine(&r,p,ram,0,native);
    assert(r.view.core_x==214 && r.view.core_y==112); /* title, both axes */
    ScRendererDestroy(&r); free(p); free(before); free(ram); free(rom);
    puts("PASS: tile flips, overlays, map bounds, native pixels, tall/wide surfaces and PPU immutability");
    return 0;
}
