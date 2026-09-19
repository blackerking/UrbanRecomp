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
static int sprite_x(const Ppu *p,int slot) {
    int index=slot*2;
    return (p->oam[index]&255)|(((p->highOam[index/8]>>(index%8))&1)<<8);
}
static unsigned sprite_pixel(const Ppu *p,int slot,int x,int y) {
    static const int sizes[8][2]={{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
    int index=slot*2, size=sizes[PPU_objSize(p)][(p->highOam[index/8]>>(index%8+1))&1];
    if (x<0 || y<0 || x>=size || y>=size) return 0;
    unsigned attr=p->oam[index+1];
    if (attr&0x4000) x=size-1-x;
    if (attr&0x8000) y=size-1-y;
    unsigned tile=(((attr&0xf0)+(y/8)*16)&255)|(((attr&15)+x/8)&15);
    unsigned base=attr&0x100 ? PPU_objTileAdr2(p) : PPU_objTileAdr1(p);
    return tile_pixel(p,tile|(((attr>>9)&7)<<10),base,x,y,4,128);
}
static void find_lights(ScRenderer *r,const Ppu *p) {
    r->light_slot=-1; r->light_pitch=0;
    if (!r->title_live) return;
    int best=2;
    for (int i=0;i<104;++i) {
        int y=p->oam[i*2]>>8, lo=256, pitch=512, count=0;
        if (y<150 || y>215) continue;
        for (int j=0;j<104;++j) {
            int x=sprite_x(p,j);
            if (x>=256 || p->oam[j*2+1]!=p->oam[i*2+1] || (p->oam[j*2]>>8)!=y) continue;
            ++count;
            if (x<lo) { if (lo-x<pitch) pitch=lo-x; lo=x; }
            else if (x>lo && x-lo<pitch) pitch=x-lo;
        }
        if (count>best && pitch>0 && pitch<=128) {
            best=count; r->light_slot=i; r->light_x=lo; r->light_pitch=pitch;
        }
    }
}
static unsigned cell_pixel(const ScRenderer *r,const Ppu *p,const uint8_t *ram,
                           int x,int y,bool overlay) {
    if (!r->rom || x<0 || y<0 || x>=960 || y>=800) return 0;
    unsigned offset_cell=((y/8)*120+x/8)*2;
    unsigned cell=(r->map_hold ? u16(r->held_map,offset_cell) : u16(ram,MAP+offset_cell))&1023;
    if (cell>=CELL_TYPES) return 0;
    size_t offset=(overlay ? OVERLAYS : TILES)+cell*2;
    if (offset+1 >= r->rom_size) return 0;
    unsigned word=u16(r->rom,offset);
    if (overlay && (word&1023)==0x300) return 0;
    return tile_pixel(p,word,PPU_bgTileAdr(p,1),x,y,4,0);
}
static int scroll_delta(int a,int b) { int d=(a-b)&255; return d>128 ? d-256 : d; }
static bool city_live(const ScRenderer *r,const Ppu *p,const uint8_t *ram) {
    return r->rom_is_us && ram[0x14]==0 && u16(ram,0x3e)!=0 &&
           PPU_mode(p)==1 && ((p->screenEnabled[0]|p->screenEnabled[1])&2) &&
           r->wood_layer<0 && !(!(p->screenEnabled[0]&2) && (p->screenEnabled[0]&1));
}
static void track_scroll(ScRenderer *r,const Ppu *p,const uint8_t *ram) {
    if (!city_live(r,p,ram)) { r->scroll_valid=false; return; }
    int h=p->hScroll[1]&255,v=p->vScroll[1]&255;
    int x=(int8_t)ram[0x1bd]*8+(h&7),y=(int8_t)ram[0x1bf]*8+(v&7);
    if (r->scroll_valid) {
        int dx=scroll_delta(h,r->scroll_h),dy=scroll_delta(v,r->scroll_v);
        if (abs(dx)<32 && abs(dy)<32) {
            int ax=dx-(x-r->scroll_x),ay=dy-(y-r->scroll_y);
            if (ax%8==0) r->scroll_adjust_x+=ax;
            if (ay%8==0) r->scroll_adjust_y+=ay;
            if (!dx && !dy && x==r->scroll_x && y==r->scroll_y) ++r->scroll_still;
            else r->scroll_still=0;
            if (r->scroll_still>=8) r->scroll_adjust_x=r->scroll_adjust_y=0;
            if (r->scroll_adjust_x>8) r->scroll_adjust_x=8;
            if (r->scroll_adjust_x< -8) r->scroll_adjust_x=-8;
            if (r->scroll_adjust_y>8) r->scroll_adjust_y=8;
            if (r->scroll_adjust_y< -8) r->scroll_adjust_y=-8;
        } else r->scroll_adjust_x=r->scroll_adjust_y=0;
    } else r->scroll_adjust_x=r->scroll_adjust_y=r->scroll_still=0;
    r->scroll_valid=true; r->scroll_x=x; r->scroll_y=y; r->scroll_h=h; r->scroll_v=v;
}
static void track_objects(ScRenderer *r,const Ppu *p,const uint8_t *ram) {
    bool city=city_live(r,p,ram);
    for (int slot=0;slot<128;++slot) {
        int raw=sprite_x(p,slot), y=p->oam[slot*2]>>8;
        unsigned attr=p->oam[slot*2+1]&0xfe00;
        int dx=(raw-r->object_raw[slot])&511; if (dx>256) dx-=512;
        int dy=scroll_delta(y,r->object_y[slot]);
        bool step=r->objects_valid && city && attr==r->object_attr[slot] && abs(dx)<=16 && abs(dy)<=16;
        if (!step) {
            r->object_grace[slot]=0;
            r->object_x[slot]=raw<256 ? raw : raw-512;
        } else {
            r->object_x[slot]+=dx;
            if (dx || dy) r->object_grace[slot]=16;
            else if (r->object_grace[slot]) --r->object_grace[slot];
        }
        r->object_raw[slot]=raw; r->object_y[slot]=y; r->object_attr[slot]=attr;
    }
    r->objects_valid=city;
}
static void track_map_swap(ScRenderer *r,const Ppu *p,const uint8_t *ram) {
    if (!city_live(r,p,ram)) { r->map_valid=r->map_hold=false; return; }
    const uint8_t *map=ram+MAP;
    bool black=PPU_forcedBlank(p) || !PPU_brightness(p);
    if (r->map_valid) {
        int step=0,total=0;
        for (int i=0;i<24000;i+=2) {
            step+=((u16(map,i)^u16(r->previous_map,i))&1023)!=0;
            total+=((u16(map,i)^u16(r->held_map,i))&1023)!=0;
        }
        if (!r->map_hold && step>300) {
            r->map_hold=true; r->map_confirmed=r->map_dark=false;
            r->map_quiet=r->map_age=0;
        }
        if (r->map_hold) {
            if (total>4000) r->map_confirmed=true;
            if (black) r->map_dark=true;
            if (step>300) r->map_quiet=0; else ++r->map_quiet;
            if ((!black && r->map_dark) || ++r->map_age>900 ||
                (!r->map_confirmed && r->map_quiet>=10)) r->map_hold=false;
        }
    }
    memcpy(r->previous_map,map,sizeof r->previous_map);
    if (!r->map_hold) {
        memcpy(r->held_map,map,sizeof r->held_map);
        memcpy(r->held_ppu,p,sizeof *p);
        r->held_x=r->scroll_x+r->scroll_adjust_x;
        r->held_y=r->scroll_y+r->scroll_adjust_y;
    }
    r->map_valid=true;
}
static void object_row(const ScRenderer *r,const Ppu *p,int y,uint16_t *pixels) {
    memset(pixels,0,(size_t)r->view.width*sizeof(*pixels));
    int first=PPU_objPriority(p) ? (p->oamaddl&0xfe)/2 : 0;
    for (int rank=127;rank>=0;--rank) {
        int slot=(first+rank)&127;
        if (!r->object_grace[slot]) continue; /* parked HUD/cursor copies */
        int row=(y+1-r->object_y[slot])&255;
        if (row>=64) continue;
        int left=r->object_x[slot]+r->view.core_x;
        for (int dx=0;dx<64;++dx) {
            int x=left+dx;
            if (x<0 || x>=r->view.width) continue;
            unsigned ci=sprite_pixel(p,slot,dx,row);
            if (ci) pixels[x]=(uint16_t)(ci|(((p->oam[slot*2+1]>>12)&3)<<8));
        }
    }
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
    if (r->title_live && PPU_mode(p)==1) {
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
        /* Cards and translated names already exist in the wide guest maps.
         * Show that single strip, then continue the desk beyond it. Wrapping
         * the entire layer would repeat cards on ultrawide displays. */
        int tx=x+p->hScroll[0], ty=(y+1+p->vScroll[0])&255;
        unsigned anchor=p->vram[0x3400+(ty/8)*32+9]; /* column 41 */
        if (!wood_tile(anchor) && r->wood_layer==0) anchor=r->wood_rows[ty/8];
        if (wood_tile(anchor)) {
            unsigned word=wood_grow(anchor,(tx>>3)-41);
            ci=tile_pixel(p,word,PPU_bgTileAdr(p,0),tx,ty,2,0); owner=0;
        }
        for (int layer=3;layer>=0;--layer) {
            int lx=x+p->hScroll[layer], ly=y+1+p->vScroll[layer];
            if (!(p->screenEnabled[0]&(1<<layer)) || lx<0 || lx>=416 || ly<0 || ly>=256) continue;
            unsigned sample=bg_pixel(p,layer,x,y+1);
            if (sample) { ci=sample; owner=layer; }
        }
    } else if (PPU_mode(p)==0 && screen!=0) {
        /* Decorated menus use a background layer. Its clear top strip is
         * repeatable furniture-free scenery, including the fax desk. */
        if ((screen==14 || screen==15) && (p->screenEnabled[0]&4)) {
            ci=bg_pixel(p,2,x,((y+1)&15)-p->vScroll[2]); owner=2;
        } else for (int layer=0; layer<4; ++layer) if (p->screenEnabled[0]&(1<<layer)) {
            ci=bg_pixel(p,layer,x,((y+1)&15)-p->vScroll[layer]); owner=layer; break;
        }
    }
    if (r->title_live && r->light_slot>=0 && (p->screenEnabled[0]&16)) {
        int dx=(x-r->light_x)%r->light_pitch;
        if (dx<0) dx+=r->light_pitch;
        unsigned light=sprite_pixel(p,r->light_slot,dx,y+1-(p->oam[r->light_slot*2]>>8));
        if (light) { ci=light; owner=4; }
    }
    return composite_color(p,ci,owner,0,5,x);
}
static bool edge_has_overlay(const Ppu *p,int y,int left) {
    for (int x=left;x<left+8;++x) {
        for (int layer=0;layer<=2;layer+=2)
            if ((p->screenEnabled[0]&(1<<layer)) &&
                (!(p->screenWindowed[0]&(1<<layer)) || !window_contains(p,layer,x)) &&
                bg_pixel(p,layer,x,y+1)) return true;
        if (!(p->screenEnabled[0]&16)) continue;
        for (int slot=0;slot<128;++slot) {
            int sx=sprite_x(p,slot); if (sx>=256) sx-=512;
            if (sprite_pixel(p,slot,x-sx,(y+1-(p->oam[slot*2]>>8))&255)) return true;
        }
    }
    return false;
}
void ScRendererInit(ScRenderer *r,const uint8_t *rom,size_t size,bool is_us) {
    memset(r,0,sizeof(*r)); r->rom=rom; r->rom_size=size; r->rom_is_us=is_us; r->wood_layer=-1;
}
bool ScRendererResize(ScRenderer *r,ScViewport v) {
    if (v.width<256 || v.height<224 || v.width>SC_MAX_CANVAS || v.height>SC_MAX_CANVAS ||
        v.core_x<0 || v.core_y<0 || v.core_x+256>v.width || v.core_y+224>v.height) return false;
    if (!r->held_ppu) { r->held_ppu=malloc(sizeof(Ppu)); if (!r->held_ppu) return false; }
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
void ScRendererDestroy(ScRenderer *r) { free(r->pixels); free(r->held_ppu); memset(r,0,sizeof(*r)); }
void ScRendererResetHistory(ScRenderer *r) {
    r->scroll_valid=r->objects_valid=r->map_valid=r->map_hold=r->title_live=false;
}
static void render_row(ScRenderer *r,const Ppu *p,const uint8_t *ram,int y) {
    uint32_t *out=r->pixels+(size_t)(y+r->view.core_y)*r->view.width;
    bool city=city_live(r,p,ram);
    r->city_frame|=city;
    /* WRAM supplies cells, the PPU supplies the 2-pixel steps. At a tile
     * boundary WRAM can lag a frame; preserve measured PPU motion then. */
    int sx=r->scroll_x+r->scroll_adjust_x+scroll_delta(p->hScroll[1],r->scroll_h);
    int sy=r->scroll_y+r->scroll_adjust_y+scroll_delta(p->vScroll[1],r->scroll_v);
    if (city && r->map_hold) {
        sx=r->held_x; sy=r->held_y;
        /* Freeze city graphics, palette and camera, but use the live fade and
         * color-math/window controls. No guest PPU state is changed. */
        r->held_ppu->inidisp=p->inidisp;
        memcpy(r->held_ppu->brightnessMult,p->brightnessMult,sizeof p->brightnessMult);
        r->held_ppu->cgadsub=p->cgadsub; r->held_ppu->cgwsel=p->cgwsel;
        r->held_ppu->fixedColor=p->fixedColor;
        memcpy(r->held_ppu->screenEnabled,p->screenEnabled,sizeof p->screenEnabled);
        memcpy(r->held_ppu->screenWindowed,p->screenWindowed,sizeof p->screenWindowed);
        r->held_ppu->windowsel=p->windowsel; r->held_ppu->wbgobjlog=p->wbgobjlog;
        r->held_ppu->window1left=p->window1left; r->held_ppu->window1right=p->window1right;
        r->held_ppu->window2left=p->window2left; r->held_ppu->window2right=p->window2right;
        p=r->held_ppu;
    }
    uint16_t objects[SC_MAX_CANVAS];
    if (city) object_row(r,p,y,objects);
    for (int x=0;x<r->view.width;++x) {
        int local=x-r->view.core_x;
        if (y>=0 && y<224 && local>=0 && local<256 &&
            !((local<8 && (r->repaired_edges[y]&1)) ||
              (local>=248 && (r->repaired_edges[y]&2)))) continue;
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
            unsigned obj=objects[x]&255, priority=objects[x]>>8;
            if (obj && (!samples[sub] || priority>=(over ? 3u : 2u)) && (p->screenEnabled[sub]&16) &&
                (!(p->screenWindowed[sub]&16) || !window_contains(p,4,edge))) {
                samples[sub]=obj; layers[sub]=obj<192 ? 6 : 4;
            }
        }
        out[x]=composite_color(p,samples[0],layers[0],samples[1],layers[1],edge);
    }
}
/* A screen-wide vote prevents a tax-panel edge or cursor from becoming a
 * stripe across the margin. Sample finished pixels so fades and math agree. */
static void fill_flat_margins(ScRenderer *r) {
    if (r->city_frame || r->wood_layer>=0 || r->title_live) return;
    int flat=0, votes[224]={0}, best=0;
    uint32_t colors[224];
    for (int y=0;y<224;++y) {
        const uint32_t *row=r->pixels+(size_t)(r->view.core_y+y)*r->view.width+r->view.core_x;
        bool same=true;
        for (int x=1;x<8;++x) same&=row[x]==row[0] && row[255-x]==row[255];
        flat+=same; colors[y]=row[255];
        for (int previous=0;previous<=y;++previous) if (colors[previous]==colors[y]) {
            votes[previous]++; if (votes[previous]>votes[best]) best=previous;
        }
    }
    if (flat<168) return;
    for (int y=0;y<r->view.height;++y) for (int x=0;x<r->view.width;++x)
        if (x<r->view.core_x || x>=r->view.core_x+256 ||
            y<r->view.core_y || y>=r->view.core_y+224)
            r->pixels[(size_t)y*r->view.width+x]=colors[best];
}
void ScRendererLine(ScRenderer *r,const Ppu *p,const uint8_t *ram,int line,const uint32_t *native) {
    if (!r->pixels || !p || !ram || !native || line<0 || line>=224) return;
    if (line==0) {
        r->city_frame=false;
        if (ram[0x14]==1) r->title_live=true;
        else if (ram[0x14]!=2 || PPU_forcedBlank(p) || !PPU_brightness(p)) r->title_live=false;
        find_wood(r,p,ram);
        find_lights(r,p);
        track_scroll(r,p,ram);
        track_objects(r,p,ram);
        track_map_swap(r,p,ram);
    }
    /* The 32-column guest tilemap stages incoming tiles in CRT overscan.
     * Reconstruct only those edge bands, and never cover UI or native OBJ. */
    r->repaired_edges[line]=0;
    if (r->view.width>256 && city_live(r,p,ram) && (p->screenEnabled[0]&2)) {
        if (!edge_has_overlay(p,line,0)) r->repaired_edges[line]|=1;
        if (!edge_has_overlay(p,line,248)) r->repaired_edges[line]|=2;
    }
    if (line==0) for (int y=-r->view.core_y;y<0;++y) render_row(r,p,ram,y);
    render_row(r,p,ram,line);
    int first=(r->repaired_edges[line]&1) ? 8 : 0;
    int end=(r->repaired_edges[line]&2) ? 248 : 256;
    memcpy(r->pixels+(size_t)(line+r->view.core_y)*r->view.width+r->view.core_x+first,
           native+first,(size_t)(end-first)*sizeof(*native));
    if (line==223) {
        for (int y=224;y<r->view.height-r->view.core_y;++y) render_row(r,p,ram,y);
        fill_flat_margins(r);
    }
}
