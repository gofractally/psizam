#!/usr/bin/env python3
"""Generate the focused PZAM loading report from two completed host cohorts."""
import argparse
import csv
import json
import statistics as stats
from pathlib import Path


def metric(rows, key):
    values = [r[key] for r in rows]
    return dict(n=len(values), median=stats.median(values), minimum=min(values), maximum=max(values))


def summarize(path):
    if not (path/'COMPLETE').exists(): raise ValueError('Incomplete cohort: '+str(path))
    config=json.loads((path/'config.json').read_text())
    jobs=json.loads((path/'jobs.json').read_text()); probes=json.loads((path/'probe.json').read_text())
    if not all(r['valid'] for r in jobs+probes): raise ValueError('Invalid measurement')
    if len(jobs)!=10*(config['loading_samples']+1) or len(probes)!=8*(config['probe_samples']+1):
        raise ValueError('Incomplete scheduled matrix')
    result=dict(config=config, path=str(path), jobs=[], probes=[],
                counts=dict(compiler_total=len(jobs),compiler_measured=sum(not r['warmup'] for r in jobs),
                            probe_total=len(probes),probe_measured=sum(not r['warmup'] for r in probes)))
    parent_host=path/'parent-host.json'
    if not parent_host.exists(): parent_host=Path(config['loading_parent'])/'config.json'
    if parent_host.exists():
        metadata=json.loads(parent_host.read_text())
        result['host']={k:metadata.get(k) for k in ['hardware','platform','machine','wasmtime']}
    for name in config['loading_workloads']:
        for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
            for trust in (['none','full'] if mode.endswith('pzam') else ['n/a']):
                rows=[r for r in jobs if not r['warmup'] and (r['workload'],r['mode'],r['trust'])==(name,mode,trust)]
                if len(rows)!=config['loading_samples']: raise ValueError('Missing compiler cell')
                item=dict(workload=name,mode=mode,trust=trust,wall_s=metric(rows,'wall_s'),
                          peak_rss_bytes=metric(rows,'peak_rss_bytes'))
                if mode.endswith('pzam'):
                    item.update(startup_ms=metric(rows,'startup_ms'),remaining_wall_s=metric(rows,'remaining_wall_s'),
                                startup_share_percent=stats.median(100*r['startup_ms']/1000/r['wall_s'] for r in rows),
                                phases_mean_ms={k:stats.mean(r['phases_ms'][k] for r in rows) for k in rows[0]['phases_ms']})
                result['jobs'].append(item)
    for mode in ['jit2-pzam','llvm-o2-pzam']:
        for variant in ['owned-double','owned-single','view-double','view-single']:
            rows=[r for r in probes if not r['warmup'] and (r['mode'],r['variant'])==(mode,variant)]
            if len(rows)!=config['probe_samples']: raise ValueError('Missing probe cell')
            result['probes'].append(dict(mode=mode,variant=variant,
                **{key:metric(rows,key) for key in ['total_ms','mmap_ms','extra_validation_ms','validation_ms',
                    'decode_select_ms','allocation_ms','copy_ms','peak_rss_bytes']},
                **{key:rows[0][key] for key in ['file_bytes','code_bytes','sections','relocations','functions','code_fnv1a64']}))
    steals=[r['cpu_steal_percent'] for r in jobs if 'cpu_steal_percent' in r and not r['warmup']]
    result['cpu_steal_percent']=dict(median=stats.median(steals),maximum=max(steals)) if steals else None
    return result


def cell(host, mode, workload='LLParser.cpp', trust='none'):
    return next(r for r in host['jobs'] if (r['mode'],r['workload'],r['trust'])==
                (mode,workload,'n/a' if mode=='cranelift-aot' else trust))


def probe(host, mode, variant):
    return next(r for r in host['probes'] if (r['mode'],r['variant'])==(mode,variant))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arm',type=Path,required=True); p.add_argument('--x86',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True); a=p.parse_args()
    hosts={'ARM Mac':summarize(a.arm),'x86 VPS':summarize(a.x86)}
    out=a.out;out.mkdir(parents=True,exist_ok=True)
    (out/'summary.json').write_text(json.dumps(hosts,indent=2)+'\n')
    with (out/'compiler-startup.csv').open('w') as f:
        w=csv.writer(f);w.writerow(['host','workload','engine','validation_passes','samples','wall_median_s','wall_min_s',
            'wall_max_s','startup_median_ms','startup_share_percent','rss_median_bytes'])
        for h,data in hosts.items():
            for r in data['jobs']:
                w.writerow([h,r['workload'],r['mode'],{'none':2,'full':1,'n/a':''}[r['trust']],r['wall_s']['n'],
                    *[r['wall_s'][k] for k in ['median','minimum','maximum']],r.get('startup_ms',{}).get('median',''),
                    r.get('startup_share_percent',''),r['peak_rss_bytes']['median']])
    with (out/'loader-probe.csv').open('w') as f:
        w=csv.writer(f);w.writerow(['host','engine','variant','samples','median_ms','min_ms','max_ms','rss_median_bytes','file_bytes','code_bytes'])
        for h,data in hosts.items():
            for r in data['probes']:
                w.writerow([h,r['mode'],r['variant'],r['total_ms']['n'],*[r['total_ms'][k] for k in ['median','minimum','maximum']],
                            r['peak_rss_bytes']['median'],r['file_bytes'],r['code_bytes']])
    with (out/'startup-phases.csv').open('w') as f:
        w=csv.writer(f);w.writerow(['host','workload','engine','validation_passes','phase','mean_ms'])
        for h,data in hosts.items():
            for r in data['jobs']:
                for phase,ms in r.get('phases_mean_ms',{}).items():
                    w.writerow([h,r['workload'],r['mode'],2 if r['trust']=='none' else 1,phase,ms])
    text=['# Clang PZAM loading: copy cost and startup share','',
          'Fresh-process, warm-filesystem-cache compiler jobs and a separate container-only probe. '
          'No gas/fuel checks or engine timer; memory and stack protections remain enabled.','',
          '## Large C++ compilation (LLParser.cpp)','',
          '| Host | Engine | Job median | Startup median | Startup share |',
          '|---|---|---:|---:|---:|']
    names={'jit2-pzam':'JIT2 PZAM','llvm-o2-pzam':'LLVM O2 PZAM','cranelift-aot':'Cranelift AOT'}
    for h,data in hosts.items():
        for mode in names:
            r=cell(data,mode)
            text.append(f"| {h} | {names[mode]} | {r['wall_s']['median']:.3f} s | "+
                        (f"{r['startup_ms']['median']:.2f} ms | {r['startup_share_percent']:.2f}% |" if mode.endswith('pzam') else 'not instrumented | - |'))
    text+=['','## Isolated container probe','',
           'Owned double validation is the current container path. View single validation borrows metadata/code '
           'and copies selected code once into new writable pages. Neither probe variant restores a runtime '
           'module, relocates code, or executes it. This is an opportunity estimate, not an integrated loader speedup.','',
           '| Host | Engine | Owned/double | View/single | Difference | Probe RSS reduction |',
           '|---|---|---:|---:|---:|---:|']
    for h,data in hosts.items():
        for mode in ['jit2-pzam','llvm-o2-pzam']:
            old=probe(data,mode,'owned-double');new=probe(data,mode,'view-single')
            text.append(f"| {h} | {names[mode]} | {old['total_ms']['median']:.2f} ms | {new['total_ms']['median']:.2f} ms | "
                        f"{old['total_ms']['median']-new['total_ms']['median']:.2f} ms | "
                        f"{(old['peak_rss_bytes']['median']-new['peak_rss_bytes']['median'])/2**20:.1f} MiB |")
    text+=['','Startup is too small to explain the large-job Cranelift gap. PSSZ views remain useful for reducing '
           'transient memory and repeated cold-instance creation; cached module reuse amortizes these costs. '
           'These measurements do not identify the cause of the remaining execution-time difference. '
           'ARM LLVM job times vary substantially between launches; startup does not explain that spread.','',
           '## Method and limits','',
           '- Counts and sample sizes are recorded per host in summary.json; randomized order after warmups. Ranges are observed min/max, not confidence intervals.',
           '- Compiler outputs must match both the reference raw SHA-256 and the normalized object SHA-256. All copied probe bytes must match the validated PSSZ view.',
           '- Profiling uses the original frozen pzam-run binaries. Startup ends immediately before guest entry; wall time also includes guest execution, process launch and teardown.',
           '- The probe uses Clang -O3, the frozen engine/psio headers, and generic anonymous writable mmap pages. Its times and RSS are not those of an integrated production view loader.',
           '- Existing --trust=full removes only the extra validation pass: pzam_load still validates before decoding.',
           '- No disk-cache flush, native compiler timing, gas/timeout comparison, service dispatch test, or production loader change is included.',
           '- LLParser.cpp: 5,348,473 preprocessed bytes / 145,305 lines; SipHash.cpp is the smaller compiler task.',
           '- Exact commands, binary/input/header hashes, raw profiles and outputs are retained in the two evidence cohorts.',
           '', '## Reproduce', '', 'See benchmarks/repro/README.md, section "PZAM loading diagnostic".']
    text+=['','## Hosts','']
    for h,data in hosts.items():
        meta=data.get('host',{})
        text.append(f"- {h}: {meta.get('hardware',{}).get('machdep.cpu.brand_string','see frozen parent cohort')}; "
                    f"{meta.get('platform',data['config']['platform'])}; {meta.get('wasmtime','see parent cohort')}.")
    (out/'report.md').write_text('\n'.join(text)+'\n')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,'axes.spines.right':False,
                         'axes.spines.left':False,'axes.grid':True,'axes.axisbelow':True,'grid.alpha':0.18})
    colors=['#3664a5','#d47728','#26826e']
    def savefig(fig,name):
        fig.savefig(out/(name+'.png'),dpi=190,bbox_inches='tight',facecolor='white');plt.close(fig)
    fig,axes=plt.subplots(1,2,figsize=(10,3.1))
    for ax,(h,data) in zip(axes,hosts.items()):
        rows=[cell(data,m) for m in names]
        vals=[r['wall_s']['median'] for r in rows]
        ax.barh(list(names.values())[::-1],vals[::-1],color=colors[::-1],height=.58)
        for i,r in enumerate(rows[::-1]):
            val=r['wall_s']['median']; low=r['wall_s']['minimum'];hi=r['wall_s']['maximum']
            ax.errorbar(val,i,xerr=[[val-low],[hi-val]],fmt='none',color='#1c2939',capsize=3,lw=1)
            ax.text(max(val,hi)+max(vals)*.04,i,f'{val:.2f} s',va='center',fontsize=9)
        ax.set_xlim(0,max(r['wall_s']['maximum'] for r in rows)*1.32);ax.set_title(h,loc='left',fontweight='bold')
        ax.set_xlabel('Complete LLParser.cpp compilation (seconds)');ax.grid(axis='y',visible=False)
    fig.tight_layout();savefig(fig,'large-compiler-jobs')
    groups=[('Map file',['mmap .pzam file']),('Extra validation',['pzam_validate (trust=none)']),
            ('Validate + owned decode',['pzam_load (pSSZ)']),('Copy native code',['memcpy code blob']),
            ('Relocations / veneers',['build reloc vector','plan veneers (aarch64)','write veneers + rewrite BL relocs','apply_relocations']),
            ('Other runtime setup',[])]
    palette=['#90a7bd','#b98bce','#3664a5','#d47728','#26826e','#bbc4cc']
    fig,ax=plt.subplots(figsize=(10,3.15));labels=[];components=[]
    for h,data in hosts.items():
        for mode in ['jit2-pzam','llvm-o2-pzam']:
            phases=cell(data,mode)['phases_mean_ms'];values=[sum(phases.get(k,0) for k in keys) for _,keys in groups[:-1]]
            values.append(sum(phases.values())-sum(values));components.append(values);labels.append(h+' / '+names[mode])
    for i,(label,values) in enumerate(zip(labels[::-1],components[::-1])):
        left=0
        for (name,_),value,c in zip(groups,values,palette):
            ax.barh(i,value,left=left,color=c,height=.58,label=name if i==0 else None);left+=value
        ax.text(left+1,i,f'{left:.1f} ms',va='center',fontsize=9)
    ax.set_yticks(range(4),labels[::-1]);ax.set_xlabel('Pre-entry startup (mean of sampled LLParser.cpp launches, milliseconds)')
    ax.set_xlim(0,max(map(sum,components))*1.14);ax.grid(axis='y',visible=False)
    ax.legend(ncol=3,loc='upper center',bbox_to_anchor=(.5,1.27),frameon=False,fontsize=8.5)
    fig.tight_layout();savefig(fig,'startup-phases')
    variants=['owned-double','owned-single','view-double','view-single']
    fig,axes=plt.subplots(1,2,figsize=(10,3.4),sharey=True)
    for ax,(h,data) in zip(axes,hosts.items()):
        for i,mode in enumerate(['jit2-pzam','llvm-o2-pzam']):
            vals=[probe(data,mode,v)['total_ms']['median'] for v in variants]
            ys=[j+(i-.5)*.33 for j in range(4)]
            ax.barh(ys,vals,height=.3,color=colors[i],label=names[mode])
            for y,value in zip(ys,vals):ax.text(value+1,y,f'{value:.1f}',va='center',fontsize=8.5)
        maximum=max(probe(data,m,v)['total_ms']['maximum'] for m in ['jit2-pzam','llvm-o2-pzam'] for v in variants)
        ax.set_xlim(0,maximum*1.15);ax.set_title(h,loc='left',fontweight='bold');ax.set_xlabel('Container probe median (milliseconds)')
        ax.set_yticks(range(4),['Owned / 2 validations','Owned / 1 validation','View / 2 validations','View / 1 validation'])
        ax.grid(axis='y',visible=False)
    axes[0].invert_yaxis()
    axes[0].legend(loc='upper center',bbox_to_anchor=(1,1.29),ncol=2,frameon=False)
    fig.tight_layout();savefig(fig,'container-probe')
    make_pdf(out,hosts,names)


def make_pdf(out,hosts,names):
    from reportlab.pdfgen import canvas
    from reportlab.lib.colors import HexColor, white
    from reportlab.lib.styles import ParagraphStyle
    from reportlab.platypus import Paragraph, Table, TableStyle
    from reportlab.lib.utils import ImageReader
    c=canvas.Canvas(str(out/'pzam-loading-report.pdf'),pagesize=(612,792))
    c.setTitle('Clang PZAM loading: copy cost and startup share')
    body=ParagraphStyle('body',fontName='Helvetica',fontSize=10,leading=14,textColor=HexColor('#263648'))
    small=ParagraphStyle('small',parent=body,fontSize=8.4,leading=11.5)
    page=0
    def start(title,subtitle):
        nonlocal page
        page+=1;c.setFillColor(HexColor('#17354b'));c.rect(0,752,612,40,fill=1,stroke=0)
        c.setFillColor(white);c.setFont('Helvetica-Bold',9);c.drawString(42,769,'PSIZAM / PERFORMANCE DIAGNOSTIC')
        c.setFillColor(HexColor('#17354b'));c.setFont('Helvetica-Bold',20);c.drawString(42,718,title)
        para(subtitle,693,small)
        c.setStrokeColor(HexColor('#cbd5df'));c.line(42,40,570,40)
        c.setFillColor(HexColor('#627486'));c.setFont('Helvetica',8)
        c.drawString(42,25,'Measured September 17, 2026 (US Central) | Fresh processes, warm filesystem cache')
        c.drawRightString(570,25,str(page))
    def para(text,y,style=body):
        item=Paragraph(text,style);_,height=item.wrap(528,720);item.drawOn(c,42,y-height);return y-height-10
    def picture(name,y,height):
        img=ImageReader(str(out/(name+'.png')));w,h=img.getSize();width=528
        height=min(height,width*h/w);c.drawImage(img,42,y-height,width=width,height=height,mask='auto');return y-height-12
    def table(rows,y,widths):
        converted=[[Paragraph(str(x),small) for x in row] for row in rows]
        t=Table(converted,colWidths=widths,hAlign='LEFT')
        t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),HexColor('#e9eff5')),('VALIGN',(0,0),(-1,-1),'TOP'),
            ('BOTTOMPADDING',(0,0),(-1,-1),7),('TOPPADDING',(0,0),(-1,-1),7),
            ('LINEBELOW',(0,0),(-1,0),.6,HexColor('#8fa4b8')),('LINEBELOW',(0,1),(-1,-1),.25,HexColor('#d9e1e8'))]))
        _,h=t.wrap(528,720);t.drawOn(c,42,y-h);return y-h-14
    start('PZAM loading is a small part of Clang time','Clang 22.1.2 compiling C++ to wasm32 objects from saved native artifacts; no CPU limits.')
    y=660
    y=para('<b>Abstract.</b> PZAM currently maps the file, decodes it into owned buffers, then copies native code into executable storage. '
           'The avoidable copy and allocations are measurable, especially in memory use. However, pre-entry startup is a small fraction '
           'of the large Clang job. Improving this path cannot close the observed compilation gap with Cranelift AOT.',y)
    rows=[['LLParser.cpp','LLVM job','LLVM startup','Startup share','Cranelift job']]
    for h,data in hosts.items():
        r=cell(data,'llvm-o2-pzam');w=cell(data,'cranelift-aot')
        rows.append([h,f"{r['wall_s']['median']:.3f} s",f"{r['startup_ms']['median']:.2f} ms",f"{r['startup_share_percent']:.2f}%",f"{w['wall_s']['median']:.3f} s"])
    y=table(rows,y,[116,99,104,101,108]);y=picture('large-compiler-jobs',y,210)
    y=para('Bars show complete job medians; whiskers show observed min/max across measured launches. '
           'PZAM uses its default two validation passes. LLVM and JIT2 load .pzam; Wasmtime loads its precompiled Cranelift artifact. '
           'No WebAssembly-to-native compilation is included in these launch times. ARM LLVM execution times vary substantially '
           'between launches; startup does not account for this spread.',y,small)
    para('<b>Practical implication.</b> Optimize guest execution for the large compiler workload. Use borrowed PSSZ data to reduce '
         'load-time allocations and memory pressure where many modules are loaded. Reusing a loaded module amortizes this cost further.',y)
    c.showPage()
    start('Where startup time goes','Original pzam-run binaries, profiled before entering the guest; default validation enabled.')
    y=picture('startup-phases',656,227)
    y=para('The breakdown includes mapping, both structural validations, owned decoding, the final native-code copy, relocation work '
           'and runtime setup. Means are used here so the stacked components add exactly. Summary tables use medians.',y,small)
    y=para('<b>Smaller input:</b> compiling the SipHash.cpp hashing routines amortizes startup over less compiler work.',y,small)
    rows=[['Host / engine','Small job','Startup','Share','Extra validation']]
    for h,data in hosts.items():
        for mode in ['jit2-pzam','llvm-o2-pzam']:
            r=cell(data,mode,'SipHash.cpp');ms=r['phases_mean_ms']['pzam_validate (trust=none)']
            rows.append([h+' / '+names[mode],f"{r['wall_s']['median']:.3f} s",f"{r['startup_ms']['median']:.2f} ms",f"{r['startup_share_percent']:.1f}%",f'{ms:.2f} ms'])
    y=table(rows,y,[184,80,80,69,115])
    y=para('<b>Duplicate validation.</b> The existing <font face="Courier">--trust=full</font> option removes only the outer pass; '
           '<font face="Courier">pzam_load</font> still validates. Both variants were run and produced identical compiler output. '
           'The phase timer is the clearest measure of this small redundant cost; whole-job variation can exceed it.',y)
    para('Startup stops immediately before guest entry. Subtracting it from process wall time leaves guest execution plus '
         'process launch before the profiler and teardown. It is not a pure guest CPU timer. Wasmtime startup is not separately instrumented.',y,small)
    c.showPage()
    start('Borrowing saves copies and transient memory','Isolated PSSZ container probe; this is not an integrated runtime loader benchmark.')
    y=picture('container-probe',657,237)
    rows=[['Host / engine','Owned, 2 passes','View, 1 pass','Time difference','Probe RSS reduction']]
    for h,data in hosts.items():
        for mode in ['jit2-pzam','llvm-o2-pzam']:
            old=probe(data,mode,'owned-double');new=probe(data,mode,'view-single')
            rows.append([h+' / '+names[mode],f"{old['total_ms']['median']:.2f} ms",f"{new['total_ms']['median']:.2f} ms",
                f"{old['total_ms']['median']-new['total_ms']['median']:.2f} ms",
                f"{(old['peak_rss_bytes']['median']-new['peak_rss_bytes']['median'])/2**20:.1f} MiB"])
    y=table(rows,y,[172,90,83,84,99])
    y=para('Each variant maps and validates the same file, then copies selected native code into new writable pages. '
           'Owned decoding materializes the whole container; the view reads its fields directly. Every copied byte is compared '
           'with the validated source after timing. Sample counts are recorded in the companion CSV; one excluded warmup per cell.',y)
    para('<b>Scope limit.</b> The view probe leaves metadata borrowed. It does not restore the runtime module, relocate code, '
         'set executable permissions, or run the guest. Differences in probe time and peak RSS show the container opportunity; '
         'they are not a promised end-to-end speedup or runtime RSS reduction. Native code still needs writable relocation storage.',y,small)
    c.showPage()
    hardware=' / '.join(data.get('host',{}).get('hardware',{}).get('machdep.cpu.brand_string',name)
                        for name,data in hosts.items())
    versions=', '.join(sorted({data.get('host',{}).get('wasmtime','Wasmtime version in provenance').split(' (')[0]
                               for data in hosts.values()}))
    start('Method, evidence and next priorities',hardware+'; '+versions+'.')
    y=661
    for title,desc in [
        ('Workloads','The same Clang 22.1.2 WebAssembly compiler compiles SipHash.cpp and LLParser.cpp to wasm32 object files at O2. '
         'LLParser.cpp contains 145,305 preprocessed lines / 5,348,473 bytes. Preprocessing is outside the timed region.'),
        ('Sampling','Trials are shuffled after warmups. Fresh processes use warm filesystem cache; caches are not flushed. '
         'The VPS pins guest processes to CPU 2. ARM scheduling is left to macOS. Other coordinated benchmark work was paused.'),
        ('Correctness',f"All {sum(d['counts']['compiler_total'] for d in hosts.values())} compiler launches across both hosts matched the reference raw and normalized object hashes. "
         f"All {sum(d['counts']['probe_total'] for d in hosts.values())} probe invocations matched every copied byte and agreed on code checksum and section identity. "
         'Tool, input and artifact identities were checked before and after each cohort.'),
        ('Frozen implementation','Compiler jobs use the original benchmark binaries and artifacts. The standalone probe uses the frozen psizam/psio '
         'headers, compiled at O3 with host Clang. Exact commands, header hashes and compiler identity are archived.'),
        ('Reading the results','No gas/fuel instrumentation or engine timer is enabled. Memory and stack protections remain active. '
         'Sample ranges describe variation, not statistical confidence. No native Clang or cold-disk result is mixed into this diagnostic.'),
        ('Priorities','1. Profile the guest-execution gap for large Clang jobs. 2. Replace owned container decoding with validated PSSZ views '
         'and keep the mapping alive for every borrowed field. 3. Eliminate redundant validation and benchmark integrated loading, including '
         'relocations, instance creation, malformed inputs and mapping lifetime.')]:
        y=para('<b>'+title+'.</b> '+desc,y,small)
    rows=[['Host','LLVM / Cranelift large-job ratio','VPS CPU steal']]
    for h,data in hosts.items():
        steal=data['cpu_steal_percent']
        rows.append([h,f"{cell(data,'llvm-o2-pzam')['wall_s']['median']/cell(data,'cranelift-aot')['wall_s']['median']:.2f}x",
                     f"median {steal['median']:.2f}%; max {steal['maximum']:.2f}%" if steal else 'not applicable'])
    y=table(rows,y,[116,252,160])
    y=para('Measured samples per cell (excluding one warmup): '+ '; '.join(f"{h}: {data['config']['loading_samples']} compiler jobs, {data['config']['probe_samples']} probes" for h,data in hosts.items())+'.',y,small)
    para('Reproduction: <font face="Courier">benchmarks/repro/pzam_loading.py</font> and '
         '<font face="Courier">pzam_loading_report.py</font>. Companion CSVs contain every summary cell; '
         '<font face="Courier">summary.json</font> includes full provenance. Evidence folders retain raw per-process profiles, '
         'commands, samples and the exact measurement harness.',y,small)
    c.showPage();c.save()


if __name__=='__main__':main()
