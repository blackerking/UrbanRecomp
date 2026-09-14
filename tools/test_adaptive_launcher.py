#!/usr/bin/env python3
"""Use the shared launcher's scripted UI to enable/select/play the built-in mod."""
import argparse
import configparser
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for key in ('exe','rom','state','artifacts'): ap.add_argument('--'+key,type=Path,required=True)
    args=ap.parse_args()
    args.artifacts.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='simcity-launcher-',dir=args.artifacts.resolve()))
    env={k:v for k,v in os.environ.items() if not k.startswith(('SC_','LNG_','SNESRECOMP_'))}
    env.update(SC_SCRIPTED_INPUT='1',SC_LIVE_CAPTURE=str(root/'canvas'),
               SC_RENDER_DUMP_AT='2320',SC_RENDER_DUMP_PATH=str(root/'presented.ppm'))
    env['LNG_SCRIPT']=('size:1280x800;wait:10;click:1200,48;wait:10;'
        'click:64,304;wait:5;click:750,424;wait:5;shot:choices.png;'
        'click:660,682;wait:5;shot:selected.png;click:1150,740;wait:30;quit')
    with (root/'launcher.log').open('wb') as log:
        p=subprocess.run([str(args.exe.resolve(strict=True)),str(args.rom.resolve(strict=True)),
            '--mods','--load-state',str(args.state.resolve(strict=True))],cwd=root,env=env,
            stdout=log,stderr=log,timeout=45)
    assert p.returncode==0,root/'launcher.log'
    config=configparser.ConfigParser(); config.read(root/'sc-video.ini')
    assert config['Widescreen']['Enabled']=='1'
    assert config['Widescreen']['Aspect']=='32:9'
    assert config['Widescreen']['Centered']=='1'
    assert (root/'rom.cfg').read_text().strip()==str(args.rom.resolve())
    canvas=Image.open(root/'canvas-684x224.ppm'); assert canvas.size==(684,224)
    canvas.save(root/'game.png')
    assert 'custom renderer: 32:9' in (root/'launcher.log').read_text()
    print(f'PASS: actual Mods toggle, 32:9 selection, Play, persistence and game launch. {root}',flush=True)


if __name__=='__main__': main()
