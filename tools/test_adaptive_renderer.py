#!/usr/bin/env python3
"""ROM-backed renderer regression. Captures and saves stay in a fresh artifact directory.

Checks CPU/WRAM/clock equality on every frame, byte-exact native pixels across
boot/menu/fax/advisor/city, all fit/preset sizes, and live city panning. Pillow
is required. Images still need visual inspection for the newly exposed world.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=Path,required=True)
    ap.add_argument('--rom',type=Path,required=True)
    ap.add_argument('--artifacts',type=Path,required=True)
    ap.add_argument('--scenario-only',action='store_true',help='Only run the populated San Francisco route')
    args=ap.parse_args()
    exe=args.exe.resolve(strict=True); rom=args.rom.resolve(strict=True)
    args.artifacts.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='simcity-wide-',dir=args.artifacts.resolve()))
    print(f'Artifacts: {root}',flush=True)
    env={k:v for k,v in os.environ.items() if not k.startswith(('SC_','LNG_','SNESRECOMP_'))}
    route=['--input','600:10:0008','--input','1000:10:0001',
           '--input','1500:10:0001','--input','2000:10:0001']
    results=[]

    def run(name,opts,frames,inputs,state=None,save=False,capture_start=0):
        path=root/name; path.mkdir()
        child=env|{'SC_DUMP_DIR':str(path),'SC_DUMP_INTERVAL':'100',
                   'SC_DUMP_START':str(capture_start),'SC_STATE_TRACE':str(path/'state.txt'),
                   'SC_RENDER_AUDIT':'1'}
        if save: child|={'SC_SAVE_AT':'2300','SC_SAVE_PATH':str(path/'city.state')}
        cmd=[str(exe),str(rom),'--qualify',str(frames),'--view-position','Center']+opts+inputs
        if state: cmd+=['--load-state',str(state)]
        with (path/'run.log').open('wb') as log:
            p=subprocess.run(cmd,cwd=path,env=child,stdout=log,stderr=log,timeout=180)
        if p.returncode: raise RuntimeError(f'{name} failed: {path / "run.log"}')
        if len((path/'state.txt').read_text().splitlines())!=frames:
            raise AssertionError(f'{name}: incomplete state trace')
        return path

    def compare(stock,wide,expected,position='Center'):
        assert (stock/'state.txt').read_bytes()==(wide/'state.txt').read_bytes(),f'guest state changed: {wide}'
        shots=sorted(stock.glob('frame_*.ppm'))
        for f in shots:
            a=Image.open(f); b=Image.open(wide/f.name)
            assert b.size==expected,(wide,b.size,expected)
            x=(b.width-256)//2 if position=='Center' else 0
            y=(b.height-224)//2 if position=='Center' else 0
            audit=json.loads((wide/(f.name+'.json')).read_text())
            assert (audit['core_x'],audit['core_y'])==(x,y)
            for row,mask in enumerate(audit['edge_repairs']):
                assert mask in range(4)
                left=8 if mask&1 else 0; right=248 if mask&2 else 256
                assert a.crop((left,row,right,row+1)).tobytes()==b.crop((x+left,y+row,x+right,y+row+1)).tobytes(),f'native pixels changed: {wide/f.name} row {row}'
        Image.open(wide/shots[-1].name).save(wide/'preview.png')
        results.append({'case':wide.name,'size':expected,'frames':len((wide/'state.txt').read_text().splitlines()),
                        'native_captures':len(shots),'state_equal':True})
        print('PASS',wide.name,expected,flush=True)

    scenario=['--input','600:10:0008','--input','800:10:0020','--input','900:10:0020',
              '--input','1000:10:0001','--input','1400:10:0001','--input','1900:10:0001',
              '--input','2300:10:0001']
    stock_scenario=run('stock-scenario',['--no-widescreen'],3600,scenario)
    wide_scenario=run('wide-scenario',['--widescreen','--aspect','32:9'],3600,scenario)
    compare(stock_scenario,wide_scenario,(684,224))
    if args.scenario_only:
        (root/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
        return
    stock=run('stock-route',['--no-widescreen'],3600,route,save=True)
    wide=run('wide-route',['--widescreen','--aspect','32:9'],3600,route)
    compare(stock,wide,(684,224))
    state=stock/'city.state'
    resumed=run('save-resume',['--no-widescreen'],300,[],state,capture_start=99)
    original=stock.joinpath('state.txt').read_text().splitlines()[2301:2601]
    restored=resumed.joinpath('state.txt').read_text().splitlines()
    assert [s.split(' ',1)[1] for s in original]==[s.split(' ',1)[1] for s in restored], 'save/load changed CPU/WRAM/clock'
    for frame in (99,199,299):
        assert Image.open(resumed/f'frame_{frame:010}.ppm').tobytes()==Image.open(stock/f'frame_{frame+2301:010}.ppm').tobytes(), 'save/load changed PPU output'
    print('PASS save/load matches uninterrupted execution for 300 frames',flush=True)
    pan=['--input','30:100:0180','--input','160:80:0120','--input','280:100:0140']
    stock_pan=run('stock-pan',['--no-widescreen'],480,pan,state)
    cases=[('16:9','1280x720',(342,224)),('21:9','2520x1080',(448,224)),
           ('32:9','3840x1080',(684,224)),('16:10','1280x800',(308,224)),
           ('4:3','800x600',(256,224)),('8:7','800x600',(256,224)),
           ('Fit','3840x1080',(684,224)),('Fit','720x1280',(256,532)),
           ('Fit','1000x1000',(256,300)),('Height','3840x1080',(684,224)),
           ('Height','720x1280',(256,224)),('Width','720x1280',(256,532)),
           ('Width','3840x1080',(256,224))]
    for i,(aspect,size,expected) in enumerate(cases):
        path=run(f'case-{i}-{aspect.replace(":","x")}',
                 ['--widescreen','--aspect',aspect,'--window-size',size],480,pan,state)
        compare(stock_pan,path,expected)
    path=run('top-left',['--widescreen','--aspect','32:9','--view-position','TopLeft'],480,pan,state)
    compare(stock_pan,path,(684,224),'TopLeft')
    (root/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'PASS: {len(results)} comparisons. Inspect previews under {root}',flush=True)


if __name__=='__main__': main()
