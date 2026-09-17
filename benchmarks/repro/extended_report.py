"""Strict summaries and report pages with separately verified build identities."""
import html
import json
import statistics as stats
from collections import defaultdict
from pathlib import Path
from report import LABELS, gm

POLICIES = ['none', 'gas', 'timeout']
POLICY_NAMES = {'none':'No CPU limits','gas':'Work budget','timeout':'Time limit'}
COLORS = {'none':'#8397A6','gas':'#C27C30','timeout':'#267CA4'}


def summarize_experiments(work, main):
    work=Path(work)
    from run import sha
    lock=Path(__file__).with_name('sources.lock.json')
    if sha(lock)!=main['config']['source_lock_sha256']:
        raise ValueError('Workload source lock differs from measured inputs')
    references=sorted({(r['project'],r['repository'],r['revision'])
                       for r in json.loads(lock.read_text())})
    def read(name): return json.loads((work/name).read_text())
    def identity(a,b):
        for tool in main['config']['tools']:
            if a[tool]['sha256'] != b[tool]['sha256']:
                raise ValueError('Engine/tool identity differs: '+tool)
    core=read('coremark/resources.json')
    if not core.get('complete') or core['workloads']!=['coremark']:
        raise ValueError('Complete CoreMark matrix required')
    identity(main['config']['tools'],core['config']['tools'])
    # Harness revisions may differ; measured engine and psio must be identical.
    manifests=[read('source-manifest.json'),read('coremark/source-manifest.json')]
    for repo in ['psizam','psio']:
        filtered=[{k:v for k,v in m[repo].items() if repo=='psio' or k.startswith(('include/','src/'))} for m in manifests]
        if filtered[0]!=filtered[1]:raise ValueError('Measured engine sources differ')
    def cells(rows, fields, count, metrics):
        if not rows or any(not r.get('valid',False) for r in rows):
            raise ValueError('Missing or invalid experiment results')
        groups=defaultdict(list)
        for r in rows:
            if not r.get('warmup',False):groups[tuple(r[f] for f in fields)].append(r)
        result={}
        for key, group in groups.items():
            if len(group)!=count:raise ValueError('Incorrect measured sample count: '+str(key))
            entry={'samples':len(group)}
            for metric in metrics:
                values=[r[metric] for r in group]
                entry[metric]=stats.median(values)
                entry[metric+'_min']=min(values);entry[metric+'_max']=max(values)
            result['/'.join(str(x) for x in key)]=entry
        return result
    if any(not r['valid'] for r in core['enforcement']+core['measurements']):
        raise ValueError('CoreMark gate failure')
    core_rows=core['measurements']
    for r in core_rows:
        if r['execution_s']<10:raise ValueError('CoreMark trial shorter than ten seconds')
        r['instructions_per_iteration']=r['process_instructions']/r['iterations']
        r['cycles_per_iteration']=r['process_cycles']/r['iterations']
        r['outside_fraction']=(r['wall_s']-r['execution_s'])/r['wall_s']
    core_cells=cells(core_rows,['mode','profile','phase'],core['samples'],
                     ['score','execution_s','iterations','peak_rss_bytes','instructions_per_iteration','cycles_per_iteration','outside_fraction'])
    expected={f'{v.split("/")[0]}/{p}/{seed}' for v in core['matrix'] for p in POLICIES for seed in ['performance','validation']}
    if set(core_cells)!=expected:raise ValueError('Incomplete CoreMark cells')
    native=read('coremark/native-coremark.json')
    if not native.get('complete') or any(r['execution_s']<10 for r in native['measurements']):
        raise ValueError('Complete ten-second native CoreMark trials required')
    native_cells=cells(native['measurements'],['phase'],native['samples'],['score','execution_s'])
    if set(native_cells)!={'performance','validation'}:raise ValueError('Missing native CoreMark seed set')
    cc=read('clang-resource-config.json');identity(main['config']['tools'],cc['tools'])
    if cc['source_manifest_sha256']!=main['source_manifest_sha256']:raise ValueError('Clang source identity differs')
    comp=read('clang-resource-compilation.json')
    comp_cells=cells(comp,['mode','threads','profile'],cc['compile_samples'],['wall_s','peak_rss_bytes','artifact_bytes'])
    expected={f'{m}/{t}/{p}' for m in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']
              for t in ([1] if m=='jit2-pzam' else cc['threads'])
              for p in (POLICIES if m=='cranelift-aot' else ['none','gas'])}
    if set(comp_cells)!=expected:raise ValueError('Incomplete Clang compilation cells')
    jobs=read('clang-resource-measurements.json')
    if any(r['object_sha256']!=cc['fixtures']['objects'][r['workload']] for r in jobs):raise ValueError('Clang object mismatch')
    job_cells=cells(jobs,['workload','mode','profile'],cc['samples'],['wall_s','peak_rss_bytes'])
    if set(job_cells)!={f'{n}/{m}/{p}' for n in cc['fixtures']['objects'] for m in cc['modes'] for p in POLICIES}:
        raise ValueError('Incomplete Clang job cells')
    bc=read('boundary-config.json');boundary=read('boundary.json')
    if bc['engine_source_manifest_sha256']!=main['source_manifest_sha256']:raise ValueError('Boundary source identity differs')
    boundary_cells=cells(boundary,['mode','profile'],bc['samples'],
                  ['guest_to_host_ns','host_to_guest_checked_ns','host_to_guest_optimized_ns'])
    if set(boundary_cells)!={f'{m}/{p}' for m in ['jit1','jit2','llvm-jit','cranelift'] for p in POLICIES}:
        raise ValueError('Incomplete boundary cells')
    validation=read('validation-evidence/summary.json')
    for repo, files in validation['engine_sources'].items():
        if any(manifests[0][repo].get(name)!=digest for name,digest in files.items()
               if '__pycache__' not in Path(name).parts):
            raise ValueError('Standard validation engine source differs')
    from service_report import summarize as summarize_services
    from provenance import service_cohort
    cohort, service_work, service_manifest = service_cohort(work)
    services=summarize_services(service_work, service_manifest)
    from size_report import summarize as summarize_sizes
    sizes=summarize_sizes(work)
    release=read('release-validation/summary.json')
    writer_manifest=read(sizes['jit1_provenance']['directory']+'/source-manifest.json')
    for repo, files in release['engine_sources'].items():
        if any(writer_manifest[repo].get(name)!=digest for name,digest in files.items()
               if '__pycache__' not in Path(name).parts):
            raise ValueError('Release validation source differs from JIT1 writer')
    return {'coremark':{'cells':core_cells,'samples':core['samples'],'enforcement_checks':len(core['enforcement']),
                       'trials':len(core_rows),'source_manifest_sha256':core['source_manifest_sha256'],
                       'native':native_cells,'native_compiler':native['compiler']},
            'clang':{'config':cc,'compilation':comp_cells,'jobs':job_cells,'trials':len(jobs)},
            'boundary':{'config':bc,'cells':boundary_cells,'trials':len(boundary)},
            'validation':validation,'services':services,'service_cohort':cohort,
            'artifact_sizes':sizes,'release_validation':release,
            'workload_references':[dict(project=p,repository=r,revision=v) for p,r,v in references]}


def render(s,out,title,para,picture,charts,phase):
    """Insert overview or detail pages into the shared PDF/HTML document."""
    import matplotlib.pyplot as plt
    from matplotlib.ticker import PercentFormatter
    from visual_report import WORKLOAD_TEXT
    ext=s['extended'];profiles=s['runtime_profiles'];names=s['workloads']
    def label(v):return LABELS[v.split('/')[0]]
    def bars(filename,labels,series,xlabel,percent=False):
        fig,ax=plt.subplots(figsize=(9,max(2.8,len(labels)*.66+1.1)),layout='constrained')
        n=len(series);height=.72/n; allvals=[v for _,values,_ in series for v in values]
        lo=min(0,min(allvals));hi=max(0,max(allvals));span=max(hi-lo,1e-5)
        for j,(legend,values,color) in enumerate(series):
            ys=[i+(j-(n-1)/2)*height for i in range(len(labels))]
            ax.barh(ys,values,height=height*.90,color=color,label=legend)
            for y,v in zip(ys,values):
                display=(f'{v:+.1f}%' if abs(v)>=.05 else '0.0%') if percent else (f'{v:.2f}' if abs(v)<10 else f'{v:.1f}')
                ax.text(v+span*.012*(1 if v>=0 else -1),y,display,va='center',ha='left' if v>=0 else 'right',fontsize=9)
        ax.set_yticks(range(len(labels)),labels);ax.invert_yaxis();ax.set_xlim(lo-span*.17 if lo<0 else 0,hi+span*.20)
        ax.set_xlabel(xlabel);ax.set_axisbelow(True);ax.grid(axis='x',color='#E7ECEF')
        if percent:ax.xaxis.set_major_formatter(PercentFormatter(xmax=100));ax.axvline(0,color='#637280',lw=.7)
        if n>1:ax.legend(loc='upper center',bbox_to_anchor=(.5,1.14),ncol=n,frameon=False,fontsize=9)
        fig.savefig(out/filename,dpi=175,bbox_inches='tight');plt.close(fig);charts.append(filename)
    def chartpage(filename,heading,description,labels,series,xlabel,notes,percent=False,height=365):
        bars(filename,labels,series,xlabel,percent)
        title(heading);para(description);picture(filename,height);para(notes,'Small')
    ref=next(v for v in profiles if v.startswith('cranelift-aot/'))
    llvm=next(v for v in profiles if v.startswith('llvm-o2-pzam/'))
    if phase=='overview':
        means={p:gm(s['cells'][n][ref+'/'+p]['median_s']/s['cells'][n][llvm+'/'+p]['median_s'] for n in names) for p in POLICIES}
        core=ext['coremark']['cells']
        service=ext['services']['service-calls']['cells']
        ours=service['llvm-aot/none'];theirs=service['cranelift-cached/none']
        call_metric='service_a_host_service_b_ns'
        call_delta=ours[call_metric]-theirs[call_metric]
        gas_delta=service['llvm-aot/gas'][call_metric]-service['cranelift-cached/gas'][call_metric]
        core_gap={p:100*(1-core['llvm-o2-pzam/'+p+'/performance']['score']/core['cranelift-aot/'+p+'/performance']['score']) for p in ['none','gas']}
        comp=ext['clang']['compilation'];large=ext['clang']['config']['runtime_workers']
        sizes=ext['artifact_sizes']['cells']
        title('Abstract: engine choice for psibase services')
        para('<b>Assessment:</b> The results support psizam as a competitive choice for services that reuse compiled code. They establish neither a universal winner nor a compelling execution-performance case for switching to Cranelift. Our strongest measured execution tier is saved LLVM code. For database-dominated applications, the remaining call gap may have little effect on user latency; extremely frequent, lightweight service calls favor Cranelift. This is a workload-dependent judgment, not proof of equal performance.')
        para(f'<b>Compute and calls:</b> Saved LLVM runs the eight compute workloads {means["none"]:.2f}x as fast as Cranelift AOT on their equal-weight geometric mean, and {means["gas"]:.2f}x with gas/fuel. CoreMark favors Cranelift: LLVM throughput is {core_gap["none"]:.1f}% lower without limits and {core_gap["gas"]:.1f}% lower with them. Neither workload mix establishes a psibase transaction-throughput ranking. Repeated scalar host queries favor LLVM ({ours["repeated_host_query_ns"]:.2f} versus {theirs["repeated_host_query_ns"]:.2f} ns). A protected service round trip favors cached Cranelift ({theirs[call_metric]:.2f} versus {ours[call_metric]:.2f} ns), a {call_delta:.1f} ns gap; the gas/fuel gap is {gas_delta:.1f} ns. The relevant competitor is its cached API, not its slower checked API.')
        para(f'<b>Practical significance:</b> As an illustration, eliminating {call_delta:.1f} ns once in a 1-microsecond transaction saves {call_delta/10:.2f}% of total time; once in a 10-microsecond transaction saves {call_delta/100:.2f}%. One million such calls accumulate {call_delta:.1f} ms of execution time. These are arithmetic scenarios, assuming unchanged surrounding work, not measured application improvements. Database access, payload handling and application logic can dilute the gap. Call-dense workloads, CPU saturation and infrastructure cost can still justify closing it.')
        para(f'<b>Preparation and deployment:</b> Preparing Clang takes {comp[f"cranelift-aot/{large}/none"]["wall_s"]:.2f} s with Cranelift versus {comp[f"llvm-o2-pzam/{large}/none"]["wall_s"]:.1f} s with LLVM at {large} workers; saved files are {sizes["clang/cranelift-aot/none"]["artifact_bytes"]/2**20:.1f} versus {sizes["clang/llvm-o2-pzam/none"]["artifact_bytes"]/2**20:.1f} MiB. psizam can use JIT2 for initial runs while LLVM AOT is prepared or loaded into cache, then use cached AOT for subsequent runs. Full AOT preparation therefore need not block initial service availability. Background compilation still consumes CPU and memory; this report measures the tiers separately, not concurrent preparation or switchover.')
        para('<b>Engineering decision:</b> Retain psizam and pursue inexpensive, measured improvements. Prioritize correctness and representative transaction measurements over costly rewrites to erase a nanosecond-scale microbenchmark deficit. Accept the residual gap when end-to-end latency and capacity targets are met; this report alone cannot certify that users will never notice it. A controlled full-LLVM-O2 comparison remains justified before settling the optimized tier.')
        para('<b>Scope:</b> One Apple M5 Max. Compute compares saved artifacts; call tests compare loaded LLVM PZAM with warm Cranelift C API modules, excluding compilation and linking. They measure scalar boundaries, not database execution, payload serialization, full IPC, concurrent load or tail latency. Canonical runtime proxies lack a matching Cranelift comparison. The report\'s LLVM O2 label means backend code generation at level 2 plus a custom IR pass sequence, not LLVM\'s full default O2 pipeline.','Small')
        title('Wasm engines: performance and development priorities')
        para('A practical guide to choosing and improving psizam execution backends. The report measures seven engine configurations, eight compute workloads, CoreMark, two real C++ compiler jobs, host queries, protected service round trips and canonical runtime bindings on one Apple M5 Max.')
        para('<b>Three separate questions:</b> How quickly does compiled code run? What does enforcing a work budget or deadline add? What does it cost to prepare, load and call that code? The charts keep these costs separate.')
        means={p:gm(s['cells'][n][ref+'/'+p]['median_s']/s['cells'][n][llvm+'/'+p]['median_s'] for n in names) for p in POLICIES}
        gas=[100*(s['relative_time'][v]['gas']-1) for v in [llvm,ref]]
        para(f'<b>Compute overview:</b> Saved LLVM O2 achieves {means["none"]:.2f}x Cranelift AOT speed without CPU limits and {means["gas"]:.2f}x with gas/fuel enabled, using equal-weight geometric means across the eight workloads. This is a workload mix, not a universal engine ranking.')
        para(f'<b>Cost of counting work:</b> The same aggregate adds {gas[0]:.1f}% for LLVM O2 and {gas[1]:.1f}% for Cranelift. A small difference between overhead percentages would not establish equal execution speed or statistical equivalence. Inspect the individual workloads and raw trial ranges before choosing a policy.')
        wins=defaultdict(int)
        for n in names:wins[min(profiles,key=lambda v:s['cells'][n][v+'/gas']['median_s'])]+=1
        para('<b>Lowest median time with work limits:</b> '+', '.join(label(v)+f' in {count} of {len(names)} compute tests' for v,count in sorted(wins.items(),key=lambda item:-item[1]))+'. Counting median wins helps show consistency, but treats a tiny lead the same as a large one; the workload bars show the size of each difference.','Small')
        core=ext['coremark']['cells'];ratio=core['llvm-o2-pzam/none/performance']['score']/core['cranelift-aot/none/performance']['score']
        para(f'<b>Mixed integer code:</b> CoreMark throughput is {ratio:.2f}x Cranelift AOT for LLVM O2 with no CPU limits. CoreMark is reported separately; its independently calibrated run lengths are not averaged into compute execution times.')
        sizes=ext['artifact_sizes']['cells']
        para('<b>Saving Clang:</b> Unmetered artifact sizes are '+', '.join(f'{sizes["clang/"+m+"/none"]["artifact_bytes"]/2**20:.2f} MiB for {name}' for m,name in [('jit1','JIT1'),('jit2-pzam','JIT2'),('llvm-o2-pzam','LLVM O2'),('cranelift-aot','Cranelift')])+'. Native-code-region footprints are measured separately from complete files.')
        para('<b>Reading order:</b> Start with the development priorities and execution-speed bars, then the workload-specific overhead pages. The remaining sections cover preparation time, memory, startup, sustained integer work, C++ compilation and host calls. The final methods page defines the budgets, validation and limits of the experiment.','Small')
        title('Where to focus engineering work')
        speedups={n:s['cells'][n][ref+'/none']['median_s']/s['cells'][n][llvm+'/none']['median_s'] for n in names}
        worst=min(speedups,key=speedups.get)
        comp=ext['clang']['compilation'];large=ext['clang']['config']['runtime_workers']
        lt=comp[f'llvm-o2-pzam/{large}/none']['wall_s'];ct=comp[f'cranelift-aot/{large}/none']['wall_s']
        jit2=next(v for v in profiles if v.startswith('jit2/'))
        simd=gm(s['cells'][n][jit2+'/none']['median_s']/s['cells'][n][llvm+'/none']['median_s'] for n in ['gemm-simd','jacobi-simd'])
        boundary=ext['boundary']['cells'];checked=boundary['jit2/none']['host_to_guest_checked_ns'];typed=boundary['jit2/none']['host_to_guest_optimized_ns']
        service=ext['services']['service-calls']['cells']
        call_priority=f"Protected A→host→B round trips cost {service['jit2/none']['service_a_host_service_b_ns']:.1f} ns for JIT2, {service['llvm-aot/none']['service_a_host_service_b_ns']:.1f} ns for LLVM O2 PZAM and {service['cranelift-cached/none']['service_a_host_service_b_ns']:.1f} ns for cached Cranelift. Profile remaining trap-entry and argument-adapter costs. Keep per-call protection and use the focused query and nested-call benchmarks before broader reruns."
        priorities=[
            ('1. Reduce protected call-entry and import overhead',call_priority),
            ('2. Optimize expensive preparation paths',f'Clang artifact creation takes {lt:.1f} s with LLVM O2 and {ct:.1f} s with Cranelift at {large} workers (no CPU limits). JIT2 can serve initial runs while AOT becomes available in cache, so this cost need not delay first service availability. Profile optimization, code generation, batching and memory to reduce background resource use and time until optimized execution.'),
            ('3. Improve code quality where application work is slow',f'LLVM O2 reaches {ratio:.2f}x Cranelift AOT CoreMark throughput. Its weakest relative compute result is {WORKLOAD_TEXT[worst]["short"]}, at {speedups[worst]:.2f}x. Use those kernels and compiler jobs to guide IR and native-code inspection, then validate changes across all workloads. Counters alone do not identify the cause.'),
            ('4. Address ARM vector execution in the fast compilers',f'Across the two SIMD workloads, JIT2 takes {simd:.1f}x the LLVM O2 execution time without CPU limits. JIT1/JIT2 ARM floating-point SIMD uses helper routines. Investigate native lowering while retaining the required floating-point semantics and complete SIMD validation.'),
        ]
        for heading,text in priorities:para('<b>'+heading+'</b><br/>'+text)
        para('<b>Release gate:</b> Keep exact compiler-output checks, scalar/SIMD suites and real gas/deadline exhaustion in standard validation. The measured compiler jobs cover two translation units; they do not prove a complete Clang self-bootstrap. Concurrent execution and other host architectures require separate acceptance runs.','Small')
        jobs=ext['clang']['jobs'];jobname='APInt.cpp'
        savings=jobs[jobname+'/cranelift-aot/none']['wall_s']-jobs[jobname+'/llvm-o2-pzam/none']['wall_s']
        if savings>0 and lt>ct:
            import math
            count=math.ceil((lt-ct)/savings)
            para(f'<b>Preparation versus reuse:</b> At {large} workers, LLVM\'s additional Clang preparation costs {lt-ct:.1f} s, while its APInt job saves {savings:.3f} s per run. Approximately {count} identical jobs would amortize that difference. This illustration assumes reusable saved code, repeated APInt-like work, no CPU limits and no other costs.','Small')
        families=[('Six scalar compute loops',[n for n in names if not n.endswith('-simd')]),
                  ('Two vector compute loops',[n for n in names if n.endswith('-simd')])]
        series=[]
        for p in ['none','gas']:
            values=[100*gm(s['cells'][n][ref+'/'+p]['median_s']/s['cells'][n][llvm+'/'+p]['median_s'] for n in group) for _,group in families]
            values.append(100*core['llvm-o2-pzam/'+p+'/performance']['score']/core['cranelift-aot/'+p+'/performance']['score'])
            values.extend(100*jobs[n+'/cranelift-aot/'+p]['wall_s']/jobs[n+'/llvm-o2-pzam/'+p]['wall_s'] for n in ['SipHash.cpp','APInt.cpp'])
            series.append((POLICY_NAMES[p],values,COLORS[p]))
        chartpage('application-families.png','Which application work favors each engine?',
                  'Saved LLVM O2 relative to Cranelift AOT, using the same inputs and matching CPU-limit settings. Separate families expose differences hidden by a single average.',
                  [name for name,_ in families]+['CoreMark: mixed integer tasks','C++ compilation: SipHash','C++ compilation: APInt'],series,
                  'LLVM speed index | matching Cranelift AOT setting = 100 | higher is better',
                  'Compute families use equal-weight geometric means of kernel speed ratios. CoreMark compares iterations per second. Compiler jobs compare total process time, including loading and object output. Each row keeps its own timer definition; the rows are not combined into one score. Values above 100 favor LLVM; below 100 favor Cranelift.',height=355)
        chartpage('gas-throughput.png','Compute speed with work limits enabled',
                  'Gas/fuel counting is enabled, with enough budget for every program to finish. The chart compares total computation speed; it does not plot just the overhead of the checks.',
                  [label(v) for v in profiles],
                  [('Speed',[100*gm(s['cells'][n][ref+'/gas']['median_s']/s['cells'][n][v+'/gas']['median_s'] for n in names) for v in profiles],COLORS['gas'])],
                  'Compute speed index | Cranelift AOT with fuel = 100 | higher is better',
                  'Equal-weight geometric means of eight workload speed ratios, with one fixed saved-artifact configuration per engine. Only the guest computation is timed. Gas and fuel have different units; budgets do not represent equivalent amounts of work.')
        chartpage('workload-speed.png','Where LLVM execution leads or trails',
                  'Each pair runs the same Wasm and input through saved LLVM O2 and Cranelift AOT. This exposes workload differences that an aggregate can hide.',
                  [WORKLOAD_TEXT[n]['short'] for n in names],
                  [(POLICY_NAMES[p],[100*s['cells'][n][ref+'/'+p]['median_s']/s['cells'][n][llvm+'/'+p]['median_s'] for n in names],COLORS[p]) for p in ['none','gas']],
                  'LLVM speed index | matching Cranelift AOT setting = 100 | higher is better',
                  '100 means equal median execution time; 200 means LLVM completes the computation in half the time. These are five-trial medians, not claims of statistical equivalence. Compiler workers affect preparation and are not separate runtime entries.')
        spread=max(100*(s['cells'][n][v+'/'+p]['max_s']-s['cells'][n][v+'/'+p]['min_s'])/s['cells'][n][v+'/'+p]['median_s'] for n in names for v in [llvm,ref] for p in ['none','gas'])
        chartpage('metering-comparison.png','LLVM and Cranelift: does counting work cost the same?',
                  'The added execution time differs by workload. These bars compare gas and fuel overhead relative to each engine\'s own unrestricted result; they do not compare absolute runtime.',
                  [WORKLOAD_TEXT[n]['short'] for n in names],
                  [(label(v),[100*(s['cells'][n][v+'/gas']['relative_time']-1) for n in names],c) for v,c in [(llvm,'#267CA4'),(ref,'#C27C30')]],
                  'Added execution time from gas/fuel | unrestricted = 0%',
                  f'The largest observed min-to-max spread among these unrestricted and gas cells is {spread:.1f}% of its median. Five trials describe variation but do not establish an equivalence margin or a statistically powered tie. Use repeated controlled runs before treating a small difference as an engineering priority.',percent=True,height=375)
        return
    if phase!='details':raise ValueError(phase)
    for metric,filename,heading,xlabel,explanation in [
        ('peak_rss_mib','compute-memory.png','Memory used by the compute processes','Peak resident memory, MiB | lower is better',
         'Peak resident memory covers the whole process, including code, runtime and compiler where present. It is not the reserved virtual address space or isolated guest heap. Values are medians of eight per-workload medians.'),
        ('outside_kernel_ms','startup-cost.png','Time spent outside the compute loop','Milliseconds outside the guest computation | lower is better',
         'For each trial, subtract the measured kernel duration from whole-process elapsed time. The remainder includes launch, loading, fresh JIT compilation, setup and teardown. It is not an isolated instantiation timer. Values are medians of eight per-workload medians.')]:
        chartpage(filename,heading,'Fresh processes with warm filesystem caches, measured under all three CPU-limit settings.',
                  [label(v) for v in profiles],[(POLICY_NAMES[p],[stats.median(s['cells'][n][v+'/'+p][metric] for n in names) for v in profiles],COLORS[p]) for p in POLICIES],
                  xlabel,explanation,height=410)
    core=ext['coremark']['cells'];modes=[v.split('/')[0] for v in profiles]
    chartpage('coremark-throughput.png','CoreMark: sustained mixed integer work',
              'CoreMark repeatedly performs linked-list operations, matrix arithmetic and input classification. Every accepted run lasts at least ten seconds and passes the required CRC checks.',
              [LABELS[m] for m in modes],[(POLICY_NAMES[p],[core[m+'/'+p+'/performance']['score'] for m in modes],COLORS[p]) for p in POLICIES],
              'CoreMark iterations per second | higher is better',
              'Three measured trials plus one excluded warmup per engine and setting, for each of the performance and validation seed sets. Iteration counts are calibrated per engine and held fixed across its CPU settings. Scores compare throughput, not the independently calibrated run durations. Integer instruction mix and data locality differ from the numerical workloads.',height=410)
    chartpage('coremark-overhead.png','CoreMark: cost of counting work and timing out',
              'The zero baseline is each engine\'s unrestricted CoreMark performance. The same engine runs the same iteration count with each setting.',
              [LABELS[m] for m in modes],[(POLICY_NAMES[p],[100*(core[m+'/'+p+'/performance']['execution_s']/core[m+'/none/performance']['execution_s']-1) for m in modes],COLORS[p]) for p in ['gas','timeout']],
              'Additional execution time vs. no CPU limits | lower is better',
              'Small negative values can reflect measurement variation. Gas/fuel counts work; page interruption or epochs enforce an elapsed deadline. The budgets remain unexhausted during these timing trials.',percent=True)
    chartpage('coremark-native.png','CoreMark: native compilation as context',
              'The same CoreMark algorithms are compiled directly for the host CPU, providing a reference for the cost of going through Wasm. CPU work limits and deadlines are disabled in this comparison.',
              ['Native C']+[LABELS[m] for m in modes],
              [('Iterations per second',[ext['coremark']['native']['performance']['score']]+[core[m+'/none/performance']['score'] for m in modes],'#267CA4')],
              'CoreMark iterations per second | higher is better',
              'Native C is a different compilation target and execution model, without the Wasm sandbox. It is context, not another Wasm engine or a limit-enabled competitor. Three measured trials and one excluded warmup use both CRC seed sets and at least ten seconds per trial. Native compiler: '+html.escape(ext['coremark']['native_compiler'].splitlines()[0])+'.',height=375)
    counters=['instructions_per_iteration','cycles_per_iteration']
    title('CoreMark: evidence for code-generation analysis')
    para('Whole-process hardware counters help determine whether a runtime gap accompanies more instructions, more cycles, or both. They are supporting evidence; they do not identify an optimization defect by themselves.')
    for metric in counters:
        filename='coremark-'+metric+'.png'
        bars(filename,['LLVM O2 PZAM','Cranelift AOT'],[(POLICY_NAMES[p],[core[m+'/'+p+'/performance'][metric]/1000 for m in ['llvm-o2-pzam','cranelift-aot']],COLORS[p]) for p in ['none','gas']],
             ('Thousands of retired instructions' if metric.startswith('instructions') else 'Thousands of elapsed cycles')+' per CoreMark iteration')
        picture(filename,195)
    frac=max(core[m+'/'+p+'/performance']['outside_fraction'] for m in ['llvm-o2-pzam','cranelift-aot'] for p in ['none','gas'])
    para(f'Counters come from macOS /usr/bin/time -l and include process setup and teardown; they are divided by the trial\'s iteration count. The largest median share of process time outside the CoreMark timer is {100*frac:.2f}% across these four cells. Counters are not isolated function measurements and may include timer-thread work.','Small')
    title('LLVM investigation plan: measure each proposed change')
    para('The measured LLVM path includes psizam\'s Wasm lowering, runtime helpers, IR optimization and native code generation. Source inspection identifies several controlled experiments that can explain the observed workload differences.')
    para('<b>IR optimization:</b> The O2 label selects LLVM backend code-generation level 2. psizam uses a custom function-level IR pass sequence with promotion, scalar replacement, common-subexpression elimination, instruction combining, control-flow simplification, value numbering and loop-invariant motion. It does not run LLVM\'s full default O2 module pipeline or a general interprocedural inliner.')
    para('<b>Proposed experiment:</b> Compare the existing pipeline with selected inlining and module-level passes on CoreMark and the compiler jobs. Track execution time, preparation time and memory together; faster runtime with sharply higher compile cost can still be unsuitable for short-lived modules.')
    para('<b>Runtime helpers:</b> Wasm global access and call-depth accounting emit helper calls. Profile the hot functions before changing these paths. Test whether direct context access or more precise helper effects improve generated code while retaining bounds, traps and call-stack limits.')
    para('<b>Host tuning:</b> The AArch64 AOT target uses an empty CPU name and reserves x18, rather than explicitly selecting the measured Apple CPU. Test host-specific tuning against the portable artifact target, documenting any reduced portability and measuring both preparation and execution.')
    para('<b>Acceptance:</b> Change one factor at a time, preserve the unrestricted/gas/timeout matrix, run exact-output and scalar/SIMD validation, then rerun sustained CoreMark and both C++ jobs. These are source-grounded investigation targets; the current measurements do not assign a fraction of the runtime gap to any one of them.','Small')
    para('Source references in the reproduction kit: source/psizam/src/llvm_ir_translator.cpp (optimization passes and helper calls), source/psizam/src/llvm_aot_compiler.cpp (target and backend level).','Small')
    comp=ext['clang']['compilation'];config=ext['clang']['config'];jobs=ext['clang']['jobs']
    entries=[('jit2-pzam',1)]+[(m,t) for m in ['llvm-o2-pzam','cranelift-aot'] for t in config['threads']]
    for metric,filename,heading,xlabel,div in [('wall_s','clang-build-time.png','Preparing Clang.wasm for execution','Compile and save Clang, seconds | lower is better',1),
                                               ('peak_rss_bytes','clang-build-memory.png','Memory needed to prepare the compiler','Peak resident memory during artifact creation, MiB',2**20)]:
        chartpage(filename,heading,
                  'Ahead-of-time compilation converts the pinned Clang.wasm module into saved native code. Four and eighteen workers expose the tradeoff between preparation time and memory.',
                  [LABELS[m]+f' / {t} '+('worker' if t==1 else 'workers') for m,t in entries],
                  [(POLICY_NAMES[p],[comp[f'{m}/{t}/{p}'][metric]/div for m,t in entries],COLORS[p]) for p in ['none','gas']],xlabel,
                  f'Median of {config["compile_samples"]} fresh compiler processes. The command includes startup, code generation and writing the artifact. A {config["compile_watchdog"]}-second watchdog allows long LLVM compilations. psizam timeouts reuse the exact no-limit code artifact; Cranelift epoch artifacts are separately measured and retained in summary.json. JIT2 can handle initial runs while LLVM AOT is prepared or loaded into cache, followed by cached AOT execution. These bars measure artifact preparation, not time to first service response under that strategy. Background contention and switchover are unmeasured; compilation and execution were timed separately.',height=390)
    for name in config['fixtures']['objects']:
        display='SipHash: a compact hashing implementation' if name.startswith('SipHash') else 'APInt: arbitrary-precision integer code'
        chartpage('clang-'+name+'-runtime.png','Running Clang: '+display,
                  'Each fresh Wasm process runs Clang -cc1 to compile one frozen preprocessed C++ translation unit into a wasm32 object at -O2. Every output must match the expected SHA-256 exactly.',
                  [LABELS[m] for m in config['modes']],[(POLICY_NAMES[p],[jobs[f'{name}/{m}/{p}']['wall_s'] for m in config['modes']],COLORS[p]) for p in POLICIES],
                  'End-to-end C++ compilation job, seconds | lower is better',
                  f'Five measured runs after one excluded warmup. Time includes process launch, Wasm loading or fresh JIT compilation, Clang execution and object output. Fresh JITs use one compiler worker; LLVM O2 and Cranelift saved code use {config["runtime_workers"]}-worker artifacts throughout. Guest Clang execution is single-threaded. This represents compiler-as-a-service latency; it is not a complete compiler bootstrap. Whole-module LLVM ORC cold Clang startup is outside this experiment.',height=390)
    chartpage('clang-job-memory.png','Memory used while compiling C++ inside Wasm',
              'Peak resident memory during the two complete C++ jobs, including any fresh engine compilation and the Clang working set.',
              [LABELS[m] for m in config['modes']],[(POLICY_NAMES[p],[stats.median(jobs[f'{n}/{m}/{p}']['peak_rss_bytes']/2**20 for n in config['fixtures']['objects']) for m in config['modes']],COLORS[p]) for p in POLICIES],
              'Peak resident memory, MiB | median of the two job medians',
              'Use this as a single-process capacity signal. It does not predict aggregate memory for concurrent tenants, copy-on-write sharing, or the effect of a warm resident compiler service.')
    boundary=ext['boundary']['cells'];bm=['jit1','jit2','llvm-jit','cranelift']
    for metric,heading,description,notes in [
        ('guest_to_host_ns','Calling from Wasm into the host','A Wasm loop calls a native integer increment function ten million times. Each returned value feeds the next call.',
         'The measurement includes the guest loop, call transition and native increment. It exercises scalar imports, not WIT records, strings, serialization or the Component Model. Integer results are checked exactly.'),
        ('host_to_guest_checked_ns','Calling Wasm through the public checked APIs','The native process invokes a Wasm integer increment function ten million times through each engine\'s checked API, with a separate protected entry for each invocation.',
         'psizam uses call_with_return with an export name; Wasmtime uses its checked C API on a cached export. The APIs do different lookup and checking work. Treat this as the cost of these developer-facing paths, not an isolated machine-code call convention.'),
        ('host_to_guest_optimized_ns','Cached calls: batched psizam entry and Wasmtime C API','The native process repeats ten million calls using each engine\'s lower-overhead cached path. psizam amortizes its trap entry across the batch.',
         'psizam caches a typed function and batches calls inside scoped_execution; Wasmtime uses wasmtime_func_call_unchecked on a cached export. Safety preconditions and entry scopes differ. This microbenchmark shows the achievable cost through these APIs; it does not establish equivalent behavior for arbitrary application calls.')]:
        chartpage('boundary-'+metric+'.png',heading,description,[LABELS.get(m,'Cranelift') for m in bm],
                  [(POLICY_NAMES[p],[boundary[m+'/'+p][metric] for m in bm],COLORS[p]) for p in POLICIES],
                  'Nanoseconds per call | lower is better',notes+' Five measured fresh processes after one excluded warmup, for every CPU setting. Limits cover each measurement batch; budget and deadline do not expire.',height=330)
    from service_report import render as render_services
    render_services(ext['services'],chartpage,title,para)
    from size_report import render as render_sizes
    render_sizes(ext['artifact_sizes'],chartpage,title,para)
    title('Correctness and release-readiness coverage')
    v=ext['validation']
    para('Performance rankings are accepted only after result checks pass. Timing does not replace the standard engine validation suites.')
    para(f'<b>Measured experiments:</b> {s["validation_checks"]} compute output comparisons and {s["enforcement_checks"]} resource checks pass. CoreMark contributes {ext["coremark"]["trials"]} CRC-validated runs across both seed sets and {ext["coremark"]["enforcement_checks"]} resource checks. All {ext["clang"]["trials"]} C++ jobs produce the expected object hashes. All {ext["boundary"]["trials"]} host-call processes pass exact integer checks. Counts include excluded warmups where applicable.')
    para('Source-matched standard-validation evidence is retained for the compute/compiler build. The current release-source acceptance results follow on the next page. All evidence includes test logs and binary identities. A replay on another host requires separate host acceptance.','Small')
    para('<b>Outstanding compatibility scope:</b> The scalar suite reports an unsupported multiple-memory memory.grow case. This remains a release-scope decision or implementation item. The benchmark workloads do not exercise every Wasm proposal, shared modules across concurrently executing instances, or blocked host calls.')
    para('<b>Next acceptance milestone:</b> Reproduce a complete Clang self-bootstrap from pinned C++ sources and SDK; verify reproducible artifacts and output hashes, then run the independent-checkout validation on each supported host architecture. The two measured C++ translation units establish a narrower compiler-execution check.','Small')
    title('Public library acceptance and release scope')
    para('Public library acceptance is recorded against the separately archived JIT1 writer and release build. These checks exercise the installed runtime and saved-code APIs; the preceding performance measurements retain their own exact build identities.')
    for entry in ext['release_validation']['checks']:
        para('<b>'+html.escape(entry['name'])+':</b> '+html.escape(entry['result']))
    title('Public runtime capabilities and remaining review')
    for heading, description in [
        ('Cached PZAM execution', 'Public instantiation loads saved JIT1, JIT2 and LLVM O2 artifacts with ordered imports, indirect calls and independent instance memory. Instances retain their code after runtime and module handles are destroyed. Saved code fixes the tier and instrumentation; incompatible resource policy is rejected.'),
        ('Resource controls', 'Public counters, budgets and interrupt requests use executing backend state. Finite budgets apply before initialization. The public timeout policy polls wall time at gas checkpoints; lower-level code-page interruption is the timeout mechanism measured in the engine comparison.'),
        ('Installed packages and diagnostics', 'Independent base and LLVM consumers use find_package. Base execution consumers load LLVM-generated PZAM without the LLVM compiler. Raw, prepared and cached modules report missing functions, signature mismatches, unsupported imports and missing dispatch implementations.'),
        ('Remaining contracts', 'Review memory, stack and compilation-policy enforcement; allocation-free public code-page deadlines; immutable code caching; instance-specific nonscalar bindings; gas-pool credits; library linking; logical WIT compatibility and dynamic cross-memory marshalling.'),
        ('Compatibility and bootstrap', 'Review full compiler/options/runtime-ABI cache identity and malformed metadata bounds. Run native acceptance on supported ARM64 and x86-64 hosts, including 4 KiB and 16 KiB page layouts. Reproduce the complete Clang self-bootstrap. The Wasm-hosted pzam compiler also needs its 32-bit context layout and exception configuration resolved.')]:
        para('<b>'+heading+':</b> '+description)
    para('The engineering/runtime-release-review.md checklist in the reproduction kit records source-review findings and test references. Working CLI paths and scalar call benchmarks establish narrower capabilities than a stable public-library contract.','Small')
