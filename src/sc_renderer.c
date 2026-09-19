#include "sc_renderer.h"
#include "snes/ppu.h"
#include <stdlib.h>
#include <string.h>

enum { MAP = 0x10200, TILES = 0x156a9, OVERLAYS = TILES - 0x77c, CELL_TYPES = 0x77c/2 };
static unsigned u16(const uint8_t *data, size_t offset) {
    return data[offset] | ((unsigned)data[offset+1] << 8);
}
static uint32_t color(const Ppu *p, unsigned index) {
    if (PPU_forcedBlank(p)) return 0xff000000;
    unsigned c = p->cgram[index & 255];
    return 0xff000000 | (uint32_t)p->brightnessMult[c & 31] << 16 |
           (uint32_t)p->brightnessMult[(c >> 5) & 31] << 8 |
           p->brightnessMult[(c >> 10) & 31];
}
static bool window_contains(const Ppu *p,int layer,int x) {
    unsigned flags=(p->windowsel>>(layer*4))&15;
    bool a=(x>=p->window1left && x<=p->window1right) != ((flags&1)!=0);
    bool b=(x>=p->window2left && x<=p->window2right) != ((flags&4)!=0);
    if (!(flags&2)) return (flags&8) ? b : false;
    if (!(flags&8)) return a;
    switch ((p->wbgobjlog>>(layer*2))&3) {
    case 0: return a||b; case 1: return a&&b; case 2: return a!=b; default: return a==b;
    }
}
static uint32_t composite_color(const Ppu *p,unsigned main,int layer,unsigned sub,int sublayer,int x) {
    if (PPU_forcedBlank(p)) return 0xff000000;
    bool win=window_contains(p,5,x<0 ? 0 : x>255 ? 255 : x);
    unsigned clip=PPU_clipMode(p), prevent=PPU_preventMathMode(p);
    unsigned c=p->cgram[main&255];
    if (clip==3 || (clip==2 && win) || (clip==1 && !win)) c=0;
    bool math=(PPU_mathEnabled(p)&(1<<layer)) &&
              !(prevent==3 || (prevent==2 && win) || (prevent==1 && !win));
    unsigned second=PPU_addSubscreen(p) && sublayer!=5 ? p->cgram[sub&255] : p->fixedColor;
    uint32_t result=0xff000000;
    for (int channel=0;channel<3;++channel) {
        int value=(c>>(channel*5))&31;
        if (math) {
            int operand=(second>>(channel*5))&31;
            value+=PPU_subtractColor(p) ? -operand : operand;
            if (PPU_halfColor(p) && (sublayer!=5 || !PPU_addSubscreen(p))) value>>=1;
            if (value<0) value=0;
            if (value>31) value=31;
        }
        result|=(uint32_t)p->brightnessMult[value]<<(16-channel*8);
    }
    return result;
}
static unsigned tile_pixel(const Ppu *p, unsigned word, unsigned base,
                           int x, int y, int depth, unsigned palette_offset) {
    int row = word & 0x8000 ? 7-(y&7) : y&7;
    int bit = word & 0x4000 ? x&7 : 7-(x&7);
    base += (word & 1023) * (4*depth) + row;
    unsigned ci=0;
    for (int plane=0; plane<depth; plane+=2) {
        unsigned bits=p->vram[(base + plane*4) & 0x7fff];
        ci |= ((bits >> bit)&1) << plane;
        ci |= ((bits >> (bit+8))&1) << (plane+1);
    }
    return ci ? ci + ((word >> 10)&7)*(1<<depth) + palette_offset : 0;
}
static unsigned cell_pixel(const ScRenderer *r,const Ppu *p,const uint8_t *ram,
                           int x,int y,bool overlay) {
    if (!r->rom || x<0 || y<0 || x>=960 || y>=800) return 0;
    unsigned cell=u16(ram,MAP+((y/8)*120+x/8)*2)&1023;
    if (cell>=CELL_TYPES) return 0;
    size_t offset=(overlay ? OVERLAYS : TILES)+cell*2;
    if (offset+1 >= r->rom_size) return 0;
    unsigned word=u16(r->rom,offset);
    if (overlay && (word&1023)==0x300) return 0;
    return tile_pixel(p,word,PPU_bgTileAdr(p,1),x,y,4,0);
}
uint32_t ScRendererMapPixel(const ScRenderer *r,const Ppu *p,const uint8_t *ram,int x,int y) {
    if (!r->rom_is_us || !r->rom || x<0 || y<0 || x>=960 || y>=800)
        return color(p,0);
    unsigned ci=cell_pixel(r,p,ram,x,y,false);
    /* Both tables reference the city CHR (BG2). Roofs extend one whole
     * cell up-left: the covering tile belongs to the southeast neighbor. */
    unsigned over=cell_pixel(r,p,ram,x+8,y+8,true);
    return color(p,over ? over : ci);
}
static unsigned bg_pixel(const Ppu *p,int layer,int x,int y) {
    int mode=PPU_mode(p);
    if (mode>1) return 0;
    int depth=mode==0 || layer==2 ? 2 : 4;
    x=(x+p->hScroll[layer])&1023; y=(y+p->vScroll[layer])&1023;
    int bits=PPU_bigTiles(p,layer) ? 4 : 3;
    unsigned addr=PPU_bgTilemapAdr(p,layer)+((y>>bits)&31)*32+((x>>bits)&31);
    if ((x&(32<<bits)) && PPU_bgTilemapWider(p,layer)) addr+=0x400;
    if ((y&(32<<bits)) && PPU_bgTilemapHigher(p,layer))
        addr+=PPU_bgTilemapWider(p,layer) ? 0x800 : 0x400;
    unsigned word=p->vram[addr&0x7fff];
    if (bits==4) {
        unsigned n=word&1023;
        if (((x&8)!=0) != ((word&0x4000)!=0)) n++;
        if (((y&8)!=0) != ((word&0x8000)!=0)) n+=16;
        word=(word&~1023u)|(n&1023);
    }
    return tile_pixel(p,word,PPU_bgTileAdr(p,layer),x,y,depth,mode==0 ? layer*32 : 0);
}
static bool wood_tile(unsigned word) {
    unsigned tile=word&1023;
    return tile>=0x20 && tile<=0x11f && !(word&0xc000);
}
static unsigned wood_grow(unsigned word,int columns) {
    return (word&~15u)|((word+columns)&15);
}
static bool wood_run(const uint16_t *map,int row,int col,int step) {
    unsigned a=map[row*32+col], b=map[row*32+col+step];
    return wood_tile(a) && wood_tile(b) && b==wood_grow(a,step);
}
/* Discover the desk from its tile pattern, including the fax and View Mode.
 * Keep this host-side: the guest's tilemaps and staging columns are untouched. */
static void find_wood(ScRenderer *r,const Ppu *p,const uint8_t *ram) {
    r->wood_layer=-1;
    if (ram[0x14]==1 || PPU_mode(p)>1) return;
    for (int layer=0;layer<(PPU_mode(p)==0 ? 4 : 3);++layer) {
        if (!((p->screenEnabled[0]|p->screenEnabled[1])&(1<<layer))) continue;
        unsigned base=PPU_bgTilemapAdr(p,layer);
        if (base+1024>0x8000) continue;
        const uint16_t *map=p->vram+base;
        int rows=0,full=0,cells=0;
        for (int y=0;y<32;++y) {
            int count=0;
            for (int x=0;x<32;++x) count+=wood_tile(map[y*32+x]);
            cells+=count; full+=count==32;
            rows+=wood_run(map,y,0,1) && wood_run(map,y,31,-1);
        }
        if (rows<8 || full<4 || cells<300) continue;
        r->wood_layer=layer; r->wood_period=16;
        for (int period=1;period<=16;period*=2) {
            int tested=0,bad=0;
            for (int y=0;y+period<32;++y) for (int x=0;x<=31;x+=31) {
                unsigned a=map[y*32+x],b=map[(y+period)*32+x];
                if (wood_tile(a) && wood_tile(b)) { ++tested; bad+=a!=b; }
            }
            if (tested>=16 && bad*10<=tested) { r->wood_period=period; break; }
        }
        for (int y=0;y<32;++y) {
            r->wood_rows[y]=0;
            for (int offset=0;offset<32;offset+=r->wood_period) {
                int row=(y+offset)&31;
                if (wood_run(map,row,0,1)) { r->wood_rows[y]=map[row*32]; break; }
                if (wood_run(map,row,31,-1)) { r->wood_rows[y]=wood_grow(map[row*32+31],-31); break; }
            }
        }
        break;
    }
}
static uint32_t scenery(const ScRenderer *r,const Ppu *p,const uint8_t *ram,int x,int y) {
    unsigned screen=ram[0x14], ci=0;
    int owner=5;
    if (screen==1 && PPU_mode(p)==1) {
        /* Title's sky and skyline are repeating scenery; title text/sprites
         * remain in the native view. Per-line palette preserves its gradient. */
        for (int layer=2; layer>=0; --layer) {
            if (!(p->screenEnabled[0]&(1<<layer))) continue;
            unsigned sample=bg_pixel(p,layer,x,y+1);
            if (sample) { ci=sample; owner=layer; }
        }
    } else if (r->wood_layer>=0 && screen!=11 && screen!=12) {
        int layer=r->wood_layer;
        int tx=(x+p->hScroll[layer])&1023, ty=(y+1+p->vScroll[layer])&255;
        unsigned word=wood_grow(r->wood_rows[ty/8],tx/8);
        int depth=PPU_mode(p)==0 || layer==2 ? 2 : 4;
        ci=tile_pixel(p,word,PPU_bgTileAdr(p,layer),tx,ty,depth,PPU_mode(p)==0 ? layer*32 : 0);
        owner=layer;
    } else if ((screen==11 || screen==12) && PPU_mode(p)==0 && PPU_bgTilemapAdr(p,0)==0x3000) {
        /* Scenario cards occupy a partially filled 64-column map. Extend the
         * same measured four-column wood block as selector_extend_tilemap,
         * without sampling its blank staging columns or altering VRAM. */
        static const unsigned wood[]={0x29,0x39,0x49,0x59,0x61,0x71,0x81,0x91};
        int tx=(x+p->hScroll[0])&511, ty=(y+1+p->vScroll[0])&255;
        unsigned word=wood[(ty/8)&7]+(((tx/8)-41)&3);
        ci=tile_pixel(p,word,PPU_bgTileAdr(p,0),tx,ty,2,0); owner=0;
    } else if (PPU_mode(p)==0 && screen!=0) {
        /* Decorated menus use a background layer. Its clear top strip is
         * repeatable furniture-free scenery, including the fax desk. */
        if ((screen==14 || screen==15) && (p->screenEnabled[0]&4)) {
            ci=bg_pixel(p,2,x,((y+1)&15)-p->vScroll[2]); owner=2;
        } else for (int layer=0; layer<4; ++layer) if (p->screenEnabled[0]&(1<<layer)) {
            ci=bg_pixel(p,layer,x,((y+1)&15)-p->vScroll[layer]); owner=layer; break;
        }
    }
    return composite_color(p,ci,owner,0,5,x);
}
void ScRendererInit(ScRenderer *r,const uint8_t *rom,size_t size,bool is_us) {
    memset(r,0,sizeof(*r)); r->rom=rom; r->rom_size=size; r->rom_is_us=is_us; r->wood_layer=-1;
}
bool ScRendererResize(ScRenderer *r,ScViewport v) {
    if (v.width<256 || v.height<224 || v.width>SC_MAX_CANVAS || v.height>SC_MAX_CANVAS ||
        v.core_x<0 || v.core_y<0 || v.core_x+256>v.width || v.core_y+224>v.height) return false;
    size_t count=(size_t)v.width*v.height;
    if (count>r->capacity) {
        uint32_t *pixels=realloc(r->pixels,count*sizeof(*pixels));
        if (!pixels) return false;
        r->pixels=pixels; r->capacity=count;
    }
    r->view=v;
    memset(r->pixels,0,count*sizeof(*r->pixels));
    return true;
}
void ScRendererDestroy(ScRenderer *r) { free(r->pixels); memset(r,0,sizeof(*r)); }
static void render_row(ScRenderer *r,const Ppu *p,const uint8_t *ram,int y) {
    uint32_t *out=r->pixels+(size_t)(y+r->view.core_y)*r->view.width;
    bool city=r->rom_is_us && ram[0x14]==0 && u16(ram,0x3e)!=0 &&
              PPU_mode(p)==1 && ((p->screenEnabled[0]|p->screenEnabled[1])&2) &&
              r->wood_layer<0 && !(!(p->screenEnabled[0]&2) && (p->screenEnabled[0]&1));
    int sx=(int16_t)u16(ram,0x1bd)*8, sy=(int16_t)u16(ram,0x1bf)*8;
    for (int x=0;x<r->view.width;++x) {
        int local=x-r->view.core_x;
        if (y>=0 && y<224 && local>=0 && local<256) continue; /* copied from native */
        if (!city) { out[x]=scenery(r,p,ram,local,y); continue; }
        unsigned ci=cell_pixel(r,p,ram,sx+local,sy+y+1,false);
        unsigned over=cell_pixel(r,p,ram,sx+local+8,sy+y+9,true);
        if (over) ci=over;
        int edge=local<0 ? 0 : local>255 ? 255 : local;
        unsigned samples[2]={0,0}; int layers[2]={5,5};
        for (int sub=0;sub<2;++sub) {
            if ((p->screenEnabled[sub]&2) &&
                (!(p->screenWindowed[sub]&2) || !window_contains(p,1,edge))) {
                samples[sub]=ci; layers[sub]=ci ? 1 : 5;
            } else if (sub && (p->screenEnabled[1]&4)) {
                int yy=y<0 ? 0 : y>223 ? 223 : y;
                samples[sub]=bg_pixel(p,2,edge,yy+1); layers[sub]=samples[sub] ? 2 : 5;
            }
        }
        out[x]=composite_color(p,samples[0],layers[0],samples[1],layers[1],edge);
    }
}
void ScRendererLine(ScRenderer *r,const Ppu *p,const uint8_t *ram,int line,const uint32_t *native) {
    if (!r->pixels || !p || !ram || !native || line<0 || line>=224) return;
    if (line==0) find_wood(r,p,ram);
    if (line==0) for (int y=-r->view.core_y;y<0;++y) render_row(r,p,ram,y);
    render_row(r,p,ram,line);
    memcpy(r->pixels+(size_t)(line+r->view.core_y)*r->view.width+r->view.core_x,
           native,256*sizeof(*native));
    if (line==223)
        for (int y=224;y<r->view.height-r->view.core_y;++y) render_row(r,p,ram,y);
}
