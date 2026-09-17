#!/usr/bin/env python3
"""Compare measured platforms, using Wasmtime as each platform's reference."""
import argparse,hashlib,json,math,statistics
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--arm',type=Path,required=True);p.add_argument('--x86',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
data={name:json.loads(path.read_text()) for name,path in [('ARM',a.arm),('x86',a.x86)]}
assert set(data['ARM']['workloads'])==set(data['x86']['workloads'])
for name in [*data['ARM']['workloads'],'coremark']:
    assert data['ARM']['config']['modules'][name+'.wasm']['sha256']==data['x86']['config']['modules'][name+'.wasm']['sha256'],name
for part in data.values():
    assert part['config']['wasmtime'].splitlines()[0].split()[1]=='48.0.2'
assert data['ARM']['extended']['clang']['config']['fixtures']==data['x86']['extended']['clang']['config']['fixtures']
assert data['ARM']['extended']['services']['service-calls']['config']['fixture_sha256']==data['x86']['extended']['services']['service-calls']['config']['fixture_sha256']
def compiler_names(s):
    paired=s['extended']['clang'].get('paired')
    return list(paired['config']['inputs'] if paired else s['extended']['clang']['config']['fixtures']['objects'])
shared_compilers=sorted(set(compiler_names(data['ARM'])) & set(compiler_names(data['x86'])),
                        key=lambda n:({'SipHash.cpp':0,'APInt.cpp':1,'LLParser.cpp':2}.get(n,3),n))
native_comparison=all(s['extended']['clang'].get('paired') for s in data.values())
if native_comparison:
    for name in shared_compilers:
        assert data['ARM']['extended']['clang']['paired']['config']['inputs'][name]['sha256']==data['x86']['extended']['clang']['paired']['config']['inputs'][name]['sha256'],name
labels={'jit1':'JIT1','jit2':'JIT2','llvm-jit':'LLVM JIT O2','jit2-pzam':'JIT2 PZAM','llvm-o2-pzam':'LLVM O2 PZAM','cranelift-jit':'Wasmtime fresh','cranelift-aot':'Wasmtime AOT'}
modes=list(labels);rows=[]
def gm(xs):return math.exp(statistics.mean(math.log(x) for x in xs))
def variant(s,mode):return next(v for v in s['runtime_profiles'] if v.split('/')[0]==mode)
for platform,s in data.items():
    ref=variant(s,'cranelift-aot')
    for mode in modes:
        v=variant(s,mode)
        for policy in ['none','gas','timeout']:
            values=[s['cells'][n][ref+'/'+policy]['median_s']/s['cells'][n][v+'/'+policy]['median_s'] for n in s['workloads']]
            rows.append(dict(platform=platform,kind='compute',mode=mode,policy=policy,relative_speed=gm(values)))
    for name in s['workloads']:
        for policy in ['none','gas','timeout']:
            v=variant(s,'llvm-o2-pzam')
            rows.append(dict(platform=platform,kind='workload',workload=name,mode='llvm-o2-pzam',policy=policy,
                relative_speed=s['cells'][name][ref+'/'+policy]['median_s']/s['cells'][name][v+'/'+policy]['median_s']))
    cells=s['extended']['services']['service-calls']['cells']
    for mode in ['jit1','jit2','llvm','llvm-aot']:
        for policy in ['none','gas','timeout']:
            for metric in ['entry_by_index_ns','repeated_host_query_ns','service_a_host_service_b_ns']:
                rows.append(dict(platform=platform,kind='calls',mode=mode,policy=policy,metric=metric,
                    relative_speed=cells['cranelift-cached/'+policy][metric]/cells[mode+'/'+policy][metric]))
    for policy in ['none','gas','timeout']:
        cells=s['extended']['coremark']['cells']
        rows.append(dict(platform=platform,kind='coremark',mode='llvm-o2-pzam',policy=policy,
            relative_speed=cells['llvm-o2-pzam/'+policy+'/performance']['score']/cells['cranelift-aot/'+policy+'/performance']['score']))
        cells=s['extended']['clang']['jobs']
        for name in shared_compilers:
            rows.append(dict(platform=platform,kind='clang',workload=name,mode='llvm-o2-pzam',policy=policy,
                relative_speed=cells[name+'/cranelift-aot/'+policy]['wall_s']/cells[name+'/llvm-o2-pzam/'+policy]['wall_s']))
    if native_comparison:
        for name in shared_compilers:
            for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
                rows.append(dict(platform=platform,kind='clang-native-penalty',workload=name,mode=mode,policy='none',
                                 native_slowdown=cells[name+'/'+mode+'/none']['native_slowdown']))
def val(platform,**fields):
    matches=[r.get('relative_speed',r.get('native_slowdown')) for r in rows if r['platform']==platform and all(r.get(k)==v for k,v in fields.items())]
    assert len(matches)==1,(platform,fields)
    return matches[0]
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,'axes.spines.right':False,'axes.spines.left':False})
charts=[]
def chart(filename,title,entries,caption,scale=100,reference=100,xlabel='Relative speed: matching Wasmtime = 100 on each host; higher is faster'):
    fig,ax=plt.subplots(figsize=(10,max(3,len(entries)*.7+1.9)),layout='constrained')
    fig.get_layout_engine().set(rect=(0,.09,1,.91))
    largest=reference
    for shift,platform,color in [(-.17,'ARM','#267CA4'),(.17,'x86','#C27C30')]:
        values=[scale*val(platform,**fields) for label,fields in entries];largest=max(largest,*values)
        ys=[i+shift for i in range(len(entries))]
        ax.barh(ys,values,.30,label='Apple M5 Max / macOS' if platform=='ARM' else 'EPYC-Turin VPS / Linux',color=color)
        for y,v in zip(ys,values):ax.text(v+reference*.01,y,f'{v:.1f}' if scale==100 else f'{v:.2f}x',va='center',fontsize=9)
    ax.set_yticks(range(len(entries)),[label for label,fields in entries]);ax.invert_yaxis()
    ax.set_xlim(0,largest*1.18);ax.axvline(reference,color='#6B7780',linestyle='--',lw=1)
    ax.grid(axis='x',color='#E7ECEF');ax.set_axisbelow(True)
    ax.set_xlabel(xlabel)
    ax.set_title(title,pad=40,loc='left',fontsize=15)
    ax.legend(loc='upper left',bbox_to_anchor=(0,1.11),ncol=2,frameon=False)
    fig.text(.01,.015,caption,fontsize=8,color='#526577')
    fig.savefig(a.out/filename,dpi=175,bbox_inches='tight');plt.close(fig);charts.append(filename)
for policy,display in [('none','Without CPU limits'),('gas','With gas/fuel')]:
    chart('platform-compute-'+policy+'.png','Eight compute workloads: '+display,
          [(labels[m],dict(kind='compute',mode=m,policy=policy)) for m in modes],
          'Equal-weight geometric mean of per-workload median speed ratios. CPU, OS, LLVM and source revisions differ.')
    chart('platform-llvm-workloads-'+policy+'.png','Saved LLVM versus Wasmtime AOT: '+display,
          [(n,dict(kind='workload',mode='llvm-o2-pzam',workload=n,policy=policy)) for n in data['ARM']['workloads']],
          'Identical Wasm inputs. Native artifacts rebuilt on each host. Ratios compare engines within a host, not ISA in isolation.')
for metric,title in [('entry_by_index_ns','Protected host-to-guest entry'),('repeated_host_query_ns','Repeated guest-to-host queries'),('service_a_host_service_b_ns','Protected A-to-host-to-B round trip')]:
    chart('platform-'+metric+'.png',title+': no CPU limits',
          [(label,dict(kind='calls',mode=mode,policy='none',metric=metric)) for mode,label in [('jit1','JIT1'),('jit2','JIT2'),('llvm','LLVM JIT O2'),('llvm-aot','LLVM O2 PZAM')]],
          'Reference: cached Wasmtime with per-entry trap protection. Tiny scalar payloads; not complete psibase IPC.')
if native_comparison:
    for name in shared_compilers:
        chart('platform-clang-'+name+'-native.png',name+': compiler penalty versus native',
              [(labels[mode],dict(kind='clang-native-penalty',workload=name,mode=mode,policy='none')) for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']],
              'Identical inputs and wasm32 output target. No CPU limits. Includes loading, setup, guest compilation and output.',
              scale=1,reference=1,xlabel='Elapsed-time multiplier: native Clang = 1 on each host; lower is better')
manifest={'inputs':{name:{'path':str(path),'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest()} for name,path in [('ARM',a.arm),('x86',a.x86)]},'rows':rows,'charts':charts,'shared_compiler_workloads':shared_compilers,'native_comparison':native_comparison}
(a.out/'platform-comparison.json').write_text(json.dumps(manifest,indent=2)+'\n')
lines=['# ARM and x86: relative engine performance','',
       'Engine speed comparisons use each platform\'s matching Wasmtime result as the reference; higher values mean faster execution. Native compiler penalty charts use native Clang as 1; lower multipliers are better. These are platform comparisons: CPU, OS, engine-build LLVM version and engine source revision differ. All eight compute Wasm inputs and the compared compiler inputs match exactly.','',
       '| Measurement | ARM LLVM / Wasmtime speed | x86 LLVM / Wasmtime speed |','| --- | ---: | ---: |']
for label,fields in [('Eight compute workloads',dict(kind='compute')),('CoreMark',dict(kind='coremark')),*[('Clang: '+name,dict(kind='clang',workload=name)) for name in shared_compilers]]:
    lines.append(f'| {label} | {val("ARM",mode="llvm-o2-pzam",policy="none",**fields):.2f}x | {val("x86",mode="llvm-o2-pzam",policy="none",**fields):.2f}x |')
lines+=['','The first row uses an equal-weight geometric mean. Other rows retain their own timer definitions. Do not combine them into a transaction-throughput score.','']
if native_comparison:
    lines+=['| Compiler job / engine | ARM time / native | x86 time / native |','| --- | ---: | ---: |']
    for name in shared_compilers:
        for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
            fields=dict(kind='clang-native-penalty',workload=name,mode=mode,policy='none')
            lines.append(f'| {name} / {labels[mode]} | {val("ARM",**fields):.2f}x | {val("x86",**fields):.2f}x |')
    lines+=['','Native reference: Clang 22.1.2 on each host, compiling identical preprocessed inputs to wasm32 objects. Lower multipliers are better. Only compiler-identification metadata differs in the objects.','']
for filename in charts:lines+=['!['+filename.removesuffix('.png').replace('-',' ')+']('+filename+')','']
(a.out/'architecture-comparison.md').write_text('\n'.join(lines))
print(a.out/'architecture-comparison.md')
