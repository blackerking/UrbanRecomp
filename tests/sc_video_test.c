#include "sc_video.h"
#include "sc_mods.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc,char **argv) {
    assert(argc==2);
    ScVideoSettings s; ScVideoDefaults(&s);
    assert(!s.enabled && s.aspect==SC_FIT);
    ScViewport v=ScVideoViewport(&s,3840,1080);
    assert(v.width==256 && v.height==224);
    s.enabled=true;
    const int sizes[][2]={{1,1},{320,240},{1280,720},{2520,1080},{3840,1080},
                         {720,1280},{1280,1280},{16384,64},{64,16384},{0,0}};
    for (int a=0;a<SC_ASPECT_COUNT;++a) for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        s.aspect=(ScAspect)a;
        v=ScVideoViewport(&s,sizes[i][0],sizes[i][1]);
        assert(v.width>=256 && v.height>=224);
        assert(v.width<=SC_MAX_CANVAS && v.height<=SC_MAX_CANVAS);
        assert(v.core_x>=0 && v.core_x+256<=v.width);
        assert(v.core_y>=0 && v.core_y+224<=v.height);
        ScVideoRect d=ScVideoDestination(v,sizes[i][0],sizes[i][1]);
        assert(d.x>=0 && d.y>=0 && d.x+d.w<=sizes[i][0] && d.y+d.h<=sizes[i][1]);
        if (d.w && d.h) {
            int x,y;
            double px=d.x+(v.core_x+128.5)*d.w/v.width;
            double py=d.y+(v.core_y+112.5)*d.h/v.height;
            assert(ScVideoToGuest(v,d,px,py,&x,&y) && x==128 && y==112);
            assert(!ScVideoToGuest(v,d,-100,-100,&x,&y));
        }
    }
    const int fixed[]={342,448,684};
    for (int i=0;i<3;++i) {
        s.aspect=(ScAspect)(SC_16_9+i); v=ScVideoViewport(&s,800,600);
        assert(v.width==fixed[i] && v.height==224);
    }
    s.aspect=SC_FIT_WIDTH; v=ScVideoViewport(&s,720,1280);
    assert(v.width==256 && v.height==532);
    s.aspect=SC_FIT_HEIGHT; v=ScVideoViewport(&s,3840,1080);
    assert(v.width==684 && v.height==224);
    const RecompLauncherCModProvider *mods=ScModsProvider(&s,argv[1]);
    RecompLauncherCModFeature feature;
    assert(mods->feature_count(NULL)==1 && mods->feature_get(NULL,0,&feature));
    assert(!mods->feature_enable(NULL,"invalid","widescreen",1));
    assert(mods->feature_enable(NULL,"sc-widescreen","widescreen",1));
    assert(!mods->feature_set_option(NULL,"sc-widescreen","widescreen","aspect","32:0"));
    assert(mods->feature_set_option(NULL,"sc-widescreen","widescreen","aspect","32:9"));
    assert(mods->feature_set_option(NULL,"sc-widescreen","widescreen","position","TopLeft"));
    for (int i=0;i<SC_ASPECT_COUNT;++i) {
        RecompLauncherCModChoice choice;
        assert(mods->feature_choice_get(NULL,"sc-widescreen","widescreen","aspect",i,&choice));
        ScAspect parsed; assert(ScParseAspect(choice.value,&parsed) && parsed==(ScAspect)i);
    }
    assert(mods->commit(NULL,NULL));
    ScVideoSettings loaded;
    assert(ScVideoLoad(&loaded,argv[1]));
    assert(loaded.enabled && loaded.aspect==SC_32_9 && !loaded.centered);
    assert(mods->feature_enable(NULL,"sc-widescreen","widescreen",0));
    assert(mods->commit(NULL,NULL));
    assert(ScVideoLoad(&loaded,argv[1]) && !loaded.enabled);
    FILE *f=fopen(argv[1],"w"); assert(f);
    fputs("Enabled=1\nAspect=invalid\n",f); fclose(f);
    assert(!ScVideoLoad(&loaded,argv[1]) && !loaded.enabled);
    remove(argv[1]); puts("PASS: fit containment, input mapping, Mods choices and atomic persistence");
    return 0;
}
