#!/usr/bin/env python3
"""Native CoreMark context, separately calibrated and CRC checked."""
import argparse
import json
import math
import random
import re
from pathlib import Path
from run import Run, save, sha


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    p.add_argument('--samples',type=int,default=3);a=p.parse_args();r=Run(a.work)
    target=r.work/'native-coremark.json'
    if target.exists():p.error('Refusing to overwrite native measurements')
    exe=r.work/'modules/coremark.native'
    if sha(exe)!=r.config['modules']['coremark.native']['sha256']:raise ValueError('Native program changed')
    seeds={'performance':['0','0','0x66'],'validation':['0x3415','0x3415','0x66']}
    def duration(out):
        match=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
        return float(match[1]) if match else 0
    times=[]
    for seed,args in seeds.items():
        rec,out,_=r.command('native-coremark-calibrate-'+seed,[exe,*args,'100000'],timeout=240,check=True)
        times.append(duration(out))
    if min(times)<=0:raise ValueError('Missing calibration timer')
    iterations=math.ceil(100000*14/min(times)/1000)*1000
    tasks=[(seed,sample) for sample in range(-1,a.samples) for seed in seeds]
    random.Random(20260916).shuffle(tasks);tasks.sort(key=lambda item:item[1]>=0)
    result={'samples':a.samples,'iterations':iterations,'binary_sha256':sha(exe),
            'compiler':r.config['native_compiler'],'measurements':[],'complete':False}
    for seed,sample in tasks:
        rec,out,_=r.command(f'native-coremark-{seed}-{sample}',[exe,*seeds[seed],str(iterations)],timeout=240)
        score=re.search(r'Iterations/Sec\s*:\s*([0-9.]+)',out);seconds=duration(out)
        valid=rec['returncode']==0 and seconds>=10 and bool(score) and 'Correct operation validated' in out
        rec.update(phase=seed,sample=sample,warmup=sample<0,valid=valid,execution_s=seconds,score=float(score[1]) if score else 0)
        result['measurements'].append(rec);save(target,result)
        print('native CoreMark',seed,sample,seconds,'PASS' if valid else 'FAIL',flush=True)
        if not valid:raise RuntimeError('Invalid native CoreMark trial')
    result['complete']=True;save(target,result)


if __name__=='__main__':main()
