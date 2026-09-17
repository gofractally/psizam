"""Current-state report, with data-derived prose and zero-based bar charts."""
import html
import json
import statistics as stats
from collections import defaultdict
from report import LABELS,gm

WORKLOAD_TEXT = {
    'fannkuch': {
        'title': 'Reordering a list of numbers', 'short': 'list reordering',
        'test': 'Fannkuch rearranges a list of 11 numbers and repeatedly reverses sections of the list. It exercises integer arithmetic, conditional branches and reads and writes to small arrays.',
        'application': 'The closest application work is searching through possible orderings, with small loops that make frequent decisions. The result is relevant when an application has similar branching and data access.'},
    'nbody': {
        'title': 'Simulating planetary motion', 'short': 'gravity simulation',
        'test': 'The n-body test advances five bodies through 5 million time steps under gravity. It repeatedly calculates forces and updates positions using floating-point arithmetic, division and square roots.',
        'application': 'Relevant to the numerical work in physics simulations. Larger simulations, collision handling and rendering add costs that this test does not measure.'},
    'nsieve': {
        'title': 'Finding prime numbers', 'short': 'prime finding',
        'test': 'The prime sieve (nsieve) marks non-prime numbers in a compact array of bits, then counts primes. It runs three searches, up to 40.96 million, 20.48 million and 10.24 million.',
        'application': 'Applications that scan and update packed flags use similar integer and bit operations. The size of the data and the order of memory accesses affect how well this result transfers.'},
    'spectral-norm': {
        'title': 'Repeated matrix-vector calculations', 'short': 'matrix-vector math',
        'test': 'The spectral-norm test estimates how much a 2,000-by-2,000 matrix can stretch a vector. Repeated matrix-vector products compute entries as needed and use floating-point division and addition.',
        'application': 'Relevant to iterative numerical solvers that repeatedly multiply matrices and vectors. This test generates matrix entries on demand; storing and reading a full matrix has different memory costs.'},
    'gemm-scalar': {
        'title': 'Matrix multiplication, scalar code', 'short': 'matrix multiplication (scalar)',
        'test': 'GEMM multiplies 512-by-512 matrices and adds a scaled result to another matrix. This scalar Wasm version expresses arithmetic on one value at a time.',
        'application': 'Matrix multiplication is used in numerical analysis and machine learning. These results apply to this Wasm implementation; specialized math libraries and accelerators require their own measurements.'},
    'gemm-simd': {
        'title': 'Matrix multiplication, vector code', 'short': 'matrix multiplication (vector)',
        'test': 'GEMM multiplies 512-by-512 matrices and adds a scaled result to another matrix. This version enables SIMD: Wasm instructions that operate on several values at once.',
        'application': 'Relevant when numerical applications can express work using vector instructions. Each engine decides how those instructions run on the host; the result includes that implementation cost.'},
    'jacobi-scalar': {
        'title': 'Updating a 2D grid, scalar code', 'short': 'grid updates (scalar)',
        'test': 'The Jacobi test performs 500 steps on a 512-by-512 grid, repeatedly averaging each interior cell with its four neighbors. This scalar Wasm version expresses arithmetic on one value at a time.',
        'application': 'Heat-diffusion models and repeated image smoothing use similar neighboring-cell calculations. This measures the grid computation; loading images, rendering and other application work add separate costs.'},
    'jacobi-simd': {
        'title': 'Updating a 2D grid, vector code', 'short': 'grid updates (vector)',
        'test': 'The Jacobi test performs 500 steps on a 512-by-512 grid, repeatedly averaging each interior cell with its four neighbors. This version enables SIMD: Wasm instructions that operate on several values at once.',
        'application': 'Relevant to heat-diffusion and image-processing loops that can use vector instructions. This measures the grid computation; application speed also depends on data movement and other processing.'},
    'coremark': {
        'title': 'Mixed integer application tasks', 'short': 'mixed integer tasks',
        'test': 'CoreMark combines linked-list operations, matrix arithmetic and a state machine that classifies input. It checks the results and runs each accepted trial for at least ten seconds.',
        'application': 'Provides a mixed integer workload for judging execution costs. An application with a different mix of tasks, memory accesses or host calls can have a different result.'},
}


def generate(s,out,extension=None):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import PercentFormatter
    from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak, Image, Table, TableStyle
    from reportlab.platypus.tableofcontents import TableOfContents
    from reportlab.lib.styles import getSampleStyleSheet,ParagraphStyle
    from reportlab.lib import colors
    from reportlab.lib.utils import ImageReader
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,'axes.spines.right':False,
                         'axes.spines.left':False,'axes.labelcolor':'#233E50','text.color':'#233E50'})
    policies=('none','gas','timeout')
    palettes={'gas':'#C27C30','timeout':'#267CA4'};profiles=s['runtime_profiles'];names=s['workloads'];charts=[]
    def label(v,compilation=False):
        m,t=v.split('/');return LABELS[m]+(' / '+t[1:]+(' worker' if t=='j1' else ' workers') if compilation and m in ['llvm-o2-pzam','cranelift-aot'] else '')
    def chart(name, labels, vals, xlabel, color='#267CA4'):
        fig,ax=plt.subplots(figsize=(9,max(2.8,len(vals)*.35+1.1)),layout='constrained')
        ax.barh(labels,vals,color=color,height=.63);ax.invert_yaxis();ax.set_xlim(0,max(vals)*1.22)
        ax.grid(axis='x',color='#E7ECEF');ax.set_axisbelow(True);ax.set_xlabel(xlabel)
        for i,v in enumerate(vals):ax.text(v+max(vals)*.02,i,f'{v:.2f}' if v<10 else f'{v:.1f}',va='center')
        fig.savefig(out/name,dpi=175,bbox_inches='tight');plt.close(fig);charts.append(name)
    def overhead_chart(name,workload=None):
        fig,ax=plt.subplots(figsize=(9,len(profiles)*.52+1.1),layout='constrained')
        values=s['relative_time'] if workload is None else {v:{p:s['cells'][workload][v+'/'+p]['relative_time'] for p in palettes} for v in profiles}
        overhead={p:[100*(values[v][p]-1) for v in profiles] for p in palettes}
        minv=min(0,min(x for vals in overhead.values() for x in vals))
        maxv=max(0,max(x for vals in overhead.values() for x in vals))
        span=max(maxv-minv,1)
        for j,(p,c) in enumerate(palettes.items()):
            vals=overhead[p];ys=[i+(j-.5)*.23 for i in range(len(profiles))]
            ax.barh(ys,vals,color=c,height=.21,label={'gas':'Work budget','timeout':'Time limit'}[p])
            for y,x in zip(ys,vals):
                text=f'{x:+.1f}%' if abs(x)>=.05 else '0.0%'
                ax.text(x+span*.012*(1 if x>=0 else -1),y,text,
                        ha='left' if x>=0 else 'right',va='center',fontsize=8)
        ax.set_yticks(range(len(profiles)),[label(v) for v in profiles]);ax.invert_yaxis()
        ax.set_xlim(minv-span*.16 if minv<0 else 0,maxv+span*.16)
        ax.xaxis.set_major_formatter(PercentFormatter(xmax=100))
        ax.axvline(0,color='#637280',lw=.8);ax.set_xlabel('Additional execution time vs. no CPU limits | lower is better')
        ax.grid(axis='x',color='#E7ECEF');ax.set_axisbelow(True);ax.legend(loc='upper center',bbox_to_anchor=(.5,1.13),ncol=2,frameon=False)
        fig.savefig(out/name,dpi=175,bbox_inches='tight');plt.close(fig);charts.append(name)
    overhead_chart('resource-overhead.png')
    reference=next((v for v in profiles if v.startswith('cranelift-aot/')),profiles[0])
    speed={v:gm([s['cells'][n][reference+'/none']['median_s']/s['cells'][n][v+'/none']['median_s'] for n in names]) for v in profiles}
    ranked=sorted(profiles,key=lambda v:speed[v],reverse=True)
    chart('unrestricted-throughput.png',[label(v) for v in ranked],[100*speed[v] for v in ranked],
          'Compute speed index | '+label(reference)+' = 100 | higher is better')
    for n in names:overhead_chart(n+'-overhead.png',n)
    compilation=defaultdict(list)
    for row in s['compilation']:compilation[(row['mode'],row['threads'],row['profile'],row['workload'])].append(row['wall_s'])
    if compilation:
        cp=list(dict.fromkeys((m,t,p) for m,t,p,n in compilation))
        chart('artifact-creation.png',[label(m+'/j'+str(t),compilation=True)+' / '+{'none':'no limits','gas':'work budget','timeout':'time limit'}[p] for m,t,p in cp],
              [stats.median(stats.median(compilation[(m,t,p,n)]) for n in names) for m,t,p in cp],
              'Compile and save, seconds | median across test medians | lower is better')
    styles=getSampleStyleSheet();styles['BodyText'].fontSize=10;styles['BodyText'].leading=14;styles['BodyText'].spaceAfter=10
    styles['Heading1'].fontSize=23;styles['Heading1'].leading=28;styles['Heading1'].textColor=colors.HexColor('#19364B')
    styles.add(ParagraphStyle(name='Small',fontName='Helvetica',fontSize=8,leading=11,spaceAfter=8,textColor=colors.HexColor('#526577')))
    comprehensive = 'extended' in s or extension is not None
    story=[];web=[];headings=[];page=0
    def para(text,style='BodyText'):
        story.append(Paragraph(text,styles[style]));web.append('<'+('h1' if style=='Heading1' else 'p')+'>'+text+'</'+('h1' if style=='Heading1' else 'p')+'>')
    def title(text):
        nonlocal page
        if page:story.append(PageBreak());web.append('</section>')
        page+=1;headings.append(text.replace('<br/>',' '));web.append(f'<section id="section-{page}">');para('PSIZAM / CRANELIFT - '+('COMPLETED BENCHMARK RESULTS' if extension else 'PERFORMANCE AND RELEASE READINESS' if 'extended' in s else 'WORK BUDGETS AND TIMEOUTS'),'Small');para(text,'Heading1');story[-1].toc_title=text.replace('<br/>',' / ')
    def picture(name,maxheight=360):
        width,height=ImageReader(str(out/name)).getSize();ratio=min(504/width,maxheight/height)
        story.append(Image(str(out/name),width=width*ratio,height=height*ratio));story.append(Spacer(1,10))
        web.append(f'<img src="{name}" alt="{name[:-4].replace("-"," ")}">')
    if extension is not None:
        extension(s,out,title,para,picture,charts,'overview')
    if 'extended' in s:
        from extended_report import render
        render(s,out,title,para,picture,charts,'overview')
    title('Added compute time from execution limits')
    para('WebAssembly (Wasm) engines can stop runaway programs by counting work or enforcing a time deadline. These bars measure the cost of enabling those limits while completing the same computation.')
    para('<b>No CPU limits:</b> no work counting or timer. <b>Work budget:</b> counts executed work, called gas in psizam and fuel in Cranelift. <b>Time limit:</b> interrupts when an elapsed-time deadline expires. All measured runs have enough budget and time to finish. Memory and call-stack protection remain enabled.','Small')
    picture('resource-overhead.png',365)
    para(f'Equal-weight geometric means across {len(names)} tests: '+', '.join(WORKLOAD_TEXT[n]['short'] for n in names)+f'. Each result uses {s["samples"]} measured fresh-process trials after one excluded warmup.')
    para('The zero line is each engine\'s unrestricted reference. +20% means 20% longer execution time. Small negative values can reflect measurement variation; they do not establish that checking makes execution faster.','Small')
    title(f'Wasm compute speed: {len(names)} tests')
    host=s['config'].get('hardware',{}).get('machdep.cpu.brand_string',s['config']['machine'])
    para('Single-threaded Wasm execution on '+html.escape(host)+', with work counting and time limits disabled. Tests: '+', '.join(WORKLOAD_TEXT[n]['short'] for n in names)+'.')
    picture('unrestricted-throughput.png',290)
    para('<b>How to read the comparison:</b> Each engine runs the same Wasm modules and inputs. For each workload, we take the median of '+str(s['samples'])+' fresh-process trials after one excluded warmup, divide the reference engine\'s time by that engine\'s time, and take the equal-weight geometric mean across workloads. '+label(reference)+' = 100; higher is faster. Compilation, loading and process startup are outside the timer. Guest execution is serial.')
    fastest=speed[ranked[0]]
    para('<b>What this means for applications:</b> This measures the compute portion of CPU-heavy Wasm jobs, such as numerical simulation, matrix multiplication and stencil processing. '+label(ranked[0])+f' leads this aggregate at {fastest:.2f}x the reference speed. If an application\'s compute achieved that same speedup, that portion would take {100*(1-1/fastest):.0f}% less time. If it accounted for half of request latency, total latency would fall about {50*(1-1/fastest):.0f}%, with all other work unchanged. This is an illustration, not a measured application result; I/O, host calls, startup and compilation need separate measurements.')
    para('<b>Terms:</b> JIT compiles code when a module is loaded; AOT prepares code ahead of execution. PZAM is psizam\'s saved-code format; O2 names a compiler optimization level. Scalar Wasm expresses arithmetic on one value at a time; SIMD instructions operate on several values at once, within the same execution thread.','Small')
    vector_note=('JIT1/JIT2 ARM floating-point SIMD uses helper routines; LLVM and Cranelift can use hardware vectors. '
                 if s['config']['machine'] in ['arm64','aarch64'] else
                 'The runner requests hardware-deterministic floating point, but x86 JIT1 currently routes scalar FP arithmetic through software helpers. Its measurements include that implementation cost. ')
    para('All compute profiles request canonical floating-point behavior. '+vector_note+'Memory and call-stack safety checks remain enabled in all profiles.','Small')
    for name in names:
        description=WORKLOAD_TEXT[name]
        title(description['title']+'<br/>Cost of work limits and timeouts')
        para(description['test'])
        picture(name+'-overhead.png',285)
        para('<b>Read the bars:</b> A work budget counts executed work (gas/fuel); a time limit enforces an elapsed deadline. Each engine\'s no-limit execution is the 0% baseline, so +20% means 20% longer. The bars show the cost of limits within each engine; the table compares actual execution times.','Small')
        para(f'<b>Execution time:</b> median of {s["samples"]} measured runs after one warmup, using identical Wasm and inputs for all three settings. Compilation, process startup and loading are excluded.','Small')
        rows=[['Engine','No CPU limits','Work budget','Time limit']]
        for v in profiles:rows.append([label(v),*[f'{s["cells"][name][v+"/"+p]["median_s"]:.5f} s' for p in policies]])
        table=Table([[Paragraph(str(x),styles['Small']) for x in row] for row in rows],colWidths=[225,93,93,93])
        table.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#E9F0F4')),('VALIGN',(0,0),(-1,-1),'TOP'),('TOPPADDING',(0,0),(-1,-1),3),('BOTTOMPADDING',(0,0),(-1,-1),3)]))
        story.append(table);web.append('<table>'+''.join('<tr>'+''.join('<td>'+str(x)+'</td>' for x in row)+'</tr>' for row in rows)+'</table>')
        para('<b>Application relevance:</b> '+description['application'],'Small')
    if compilation:
        title('Time to compile and save executable code')
        workers=sorted({r['threads'] for r in s['compilation']})
        para('Ahead-of-time compilation (AOT) prepares native code before an application runs. PZAM is psizam\'s saved-code format. This chart measures preparing those files for the compute tests, using compiler worker budgets of '+', '.join(map(str,workers))+'.')
        picture('artifact-creation.png',390)
        para('Full command time includes process startup, compilation and writing the saved file. Each value is the median of per-test medians. Raw trials, file hashes and sizes are retained in resources.json. Requested workers are a budget; small modules may not use all workers.')
        para('LLVM O2 uses per-function compilation with one worker and batched compilation with multiple workers. That comparison includes batching and scheduling effects. No heavy compilation runs concurrently with execution timing.','Small')
    if 'extended' in s:
        render(s,out,title,para,picture,charts,'details')
    if extension is not None:
        extension(s,out,title,para,picture,charts,'details')
    if 'platform_reference' in s:
        comparison=json.loads((out/'platform-comparison/platform-comparison.json').read_text())
        title('How relative performance changes across platforms')
        para('Each host supplies its own Wasmtime reference. A value of 1.20x means psizam achieves 20% higher throughput for the measured work on that host. Comparing these ratios shows whether psizam becomes more competitive in the recorded VPS environment.')
        para('These are platform comparisons, not isolated instruction-set measurements: CPU, operating system, LLVM version and engine revision differ. All eight compute Wasm binaries, CoreMark, the service-call fixture and both compiler-job fixture manifests match exactly. The Wasmtime version is also identical.','Small')
        rows=[['Saved LLVM / Wasmtime speed','Apple M5 Max','EPYC-Turin VPS']]
        metrics=[('Eight compute workloads',{'kind':'compute'}),('CoreMark',{'kind':'coremark'}),
                 ('Clang: SipHash C++ job',{'kind':'clang','workload':'SipHash.cpp'}),
                 ('Clang: APInt C++ job',{'kind':'clang','workload':'APInt.cpp'})]
        for name,fields in metrics:
            values=[]
            for platform in ['ARM','x86']:
                matches=[r['relative_speed'] for r in comparison['rows']
                         if r['platform']==platform and r['mode']=='llvm-o2-pzam' and r['policy']=='none'
                         and all(r.get(k)==v for k,v in fields.items())]
                if len(matches)!=1:raise ValueError('Missing platform comparison: '+name)
                values.append(f'{matches[0]:.2f}x')
            rows.append([name,*values])
        table=Table([[Paragraph(x,styles['Small']) for x in row] for row in rows],colWidths=[270,117,117])
        table.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#E9F0F4')),('VALIGN',(0,0),(-1,-1),'TOP'),('TOPPADDING',(0,0),(-1,-1),5),('BOTTOMPADDING',(0,0),(-1,-1),5)]))
        story.append(table);web.append('<table>'+''.join('<tr>'+''.join('<td>'+x+'</td>' for x in row)+'</tr>' for row in rows)+'</table>')
        para('No CPU limits in this table. Compute uses an equal-weight geometric mean; other rows retain their own timer definitions. These measures must not be combined into a transaction-throughput score. The following bars separate compute, resource limits and service-call paths.','Small')
        descriptions={
            'platform-compute-none.png':'Compute speed across platforms: no CPU limits',
            'platform-compute-gas.png':'Compute speed across platforms: gas and fuel',
            'platform-llvm-workloads-none.png':'LLVM by workload across platforms: no CPU limits',
            'platform-llvm-workloads-gas.png':'LLVM by workload across platforms: gas and fuel',
            'platform-entry_by_index_ns.png':'Protected guest entry across platforms',
            'platform-repeated_host_query_ns.png':'Repeated host queries across platforms',
            'platform-service_a_host_service_b_ns.png':'Protected service round trips across platforms'}
        for name in comparison['charts']:
            title(descriptions[name]);picture('platform-comparison/'+name,390)
            charts.append('platform-comparison/'+name)
            para('Wasmtime is 100 on each host. Higher bars favor psizam; a higher VPS bar than its ARM counterpart indicates improved relative performance for that measurement. Compute uses Wasmtime AOT. Call tests use the cached Wasmtime API with per-entry trap protection and tiny scalar payloads. These are not complete psibase IPC measurements.','Small')
    title('How the tests were run and what they cover')
    config=s['config'];hardware=config.get('hardware',{}).get('machdep.cpu.brand_string',config['machine'])
    para(html.escape(hardware+' | '+config['platform']+' | '+config['date_utc']))
    if comprehensive:
        para('Build toolchain: '+html.escape(config['wasi_compiler'].splitlines()[0])+' (WASI SDK); '+html.escape(config['native_compiler'].splitlines()[0])+'. Workloads come from the pinned LLVM test suite and EEMBC CoreMark sources. Revisions, file hashes, build commands and local instrumentation are retained with the benchmark evidence.','Small')
    para(f'<b>Limits:</b> gas budget {s["gas_budget"]:,}; wall deadline {s["timeout_ms"]/1000:g} seconds, armed for every timeout trial. psizam uses code-page interruption for JIT1, JIT2, LLVM JIT and saved PZAM. Wasmtime uses epoch interruption. None of these is a process-CPU-time quota.')
    para('Gas/fuel units differ between engines and are not equivalent amounts of work. Performance budgets are deliberately non-exhausting. Both instrumented execution and actual exhaustion are tested. Timeout profiles omit gas accounting; the no-check profile omits both resource mechanisms.')
    para(f'<b>Correctness:</b> {s["enforcement_checks"]} resource checks pass, including finite completion, infinite loops and nonterminating module initialization. psizam must report its exact gas/timeout exception; Wasmtime must report its fuel/interrupt trap. The outer {s["outer_watchdog_s"]}-second process watchdog is distinct and never counts as successful enforcement.')
    para(f'{s["validation_checks"]} output comparisons pass against native output where applicable. PolyBench compares its printed arrays; this is not bitwise floating-point equality. CoreMark, if included, requires both CRC seed sets and at least ten seconds per accepted run.')
    placement=config.get('placement_notes','The machine remains a desktop environment; CPU placement, frequency and temperature are not fixed.')
    para('Execution trials run serially in fixed-seed randomized order, after one excluded warmup per cell. Artifact creation uses '+str(s['compile_samples'])+' measured fresh-process trials with no excluded warmup; small-module builds follow the fixed matrix order, while Clang builds are randomized when included. Filesystem caches are warm. '+html.escape(placement)+' Medians and observed minima/maxima are in summary.json; raw commands and output remain in the work directory. No statistical significance claim is made.','Small')
    runtime_workers=sorted({int(v.split('/j')[1]) for v in profiles})
    selection='the one-worker build' if runtime_workers==[1] else 'the lowest compiler worker count available for each engine'
    para('Compute runtime charts and tables show one entry per engine, using '+selection+' consistently across tests and limit settings. Build parallelism is compared on the compilation page. Measurements from all compiler configurations remain in the raw data; they are not pooled into runtime results.','Small')
    para(s.get('scope_text') or ('This report covers eight compute workloads, sustained CoreMark, Clang artifact creation and two C++ jobs, and scalar host-call microbenchmarks. It does not measure a complete Clang bootstrap, cold-cache deployment, concurrent instances, or blocked host calls. ' if 'extended' in s else 'This report covers only the workloads listed in its charts. It does not measure a complete Clang bootstrap, host-call microbenchmarks, cold-cache deployment, concurrent instances, or blocked host calls. ')+'Page interruption is exercised with one execution owning its module.','Small')
    if 'extended' in s:
        title('Source identities and reproducibility')
        clang_workers=s['extended']['clang']['config']['runtime_workers']
        para(f'Compute, CoreMark and compiler jobs use the same frozen CLI binaries. The supplementary scalar-boundary runner matches that source; the natural-signature service and canonical-proxy experiments use their independently archived public-runtime build. CoreMark has three measured trials for each seed set; C++ jobs and host calls have five; artifact creation has three. Clang runtime uses the {clang_workers}-worker saved builds; compute runtime uses the one-worker saved builds. The reproduction kit records each experiment separately and retains all commands and outputs. Compiler-worker counts are never pooled or shown as extra runtime engines.','Small')
    if 'extended' in s:
        e=s['extended']; writer=e['artifact_sizes']['jit1_provenance']
        para('Service-call source commit: '+e['service_cohort']['commit']+'; manifest: '+e['service_cohort']['source_manifest_sha256']+'.<br/>JIT1 saved-file writer / release source commit: '+writer['config']['psizam']['commit']+'; manifest: '+writer['source_manifest_sha256']+'. Each experiment retains its own source archive, compiler hash and inputs. No different builds are pooled within a timing cell.','Small')
        for source in e['workload_references']:
            url='https://github.com/'+source['repository']+'/tree/'+source['revision']
            para('<b>Workload source:</b> <a href="'+html.escape(url)+'" color="#267CA4">'+html.escape(source['repository'])+'</a>, revision '+source['revision']+'. Individual file hashes and local benchmark instrumentation are retained in the reproduction kit.','Small')
    para('Source base commit: '+config['psizam']['commit']+'. The measured state includes the captured working-tree changes; the frozen source manifest identifies the complete snapshot.<br/>Source manifest SHA-256: '+s['source_manifest_sha256']+'<br/>'+html.escape(config['wasmtime']),'Small')
    def footer(canvas,doc):
        canvas.setFont('Helvetica',8);canvas.setFillColor(colors.HexColor('#617586'));canvas.drawString(54,29,'PSIZAM / CRANELIFT | '+('Interim benchmark report' if extension else 'Comprehensive performance report' if 'extended' in s else 'Work budgets and timeouts'));canvas.drawRightString(558,29,str(doc.page))
    class ReportDocument(SimpleDocTemplate):
        def afterFlowable(self,flowable):
            if isinstance(flowable,Paragraph) and flowable.style.name=='Heading1':
                heading=getattr(flowable,'toc_title',flowable.getPlainText());key='section-'+str(self.page)
                self.canv.bookmarkPage(key);self.canv.addOutlineEntry(heading,key,level=0)
                if heading!='Contents':self.notify('TOCEntry',(0,heading,self.page,key))
    doc=ReportDocument(str(out/'psizam-cranelift-report.pdf'),pagesize=(612,792),rightMargin=54,leftMargin=54,topMargin=43,bottomMargin=48,title='psizam and Cranelift: '+('completed benchmark results' if extension else 'comprehensive performance and release readiness' if 'extended' in s else 'compute speed, work budgets and timeouts'))
    if comprehensive:
        toc=TableOfContents();toc.levelStyles=[ParagraphStyle(name='ContentsEntry',fontName='Helvetica',fontSize=8.5,leading=10,spaceBefore=0,textColor=colors.HexColor('#19364B'))]
        toc.tableStyle.add('TOPPADDING',(0,0),(-1,-1),0)
        toc.tableStyle.add('BOTTOMPADDING',(0,0),(-1,-1),0)
        first_break=next(i for i,f in enumerate(story) if isinstance(f,PageBreak))
        story[first_break:first_break]=[PageBreak(),Paragraph('Contents',styles['Heading1']),Spacer(1,8),toc]
        doc.multiBuild(story,onFirstPage=footer,onLaterPages=footer)
    else:doc.build(story,onFirstPage=footer,onLaterPages=footer)
    navigation='<details><summary>Report contents</summary><ol>'+''.join(f'<li><a href="#section-{i}">{html.escape(h)}</a></li>' for i,h in enumerate(headings,1))+'</ol></details>'
    (out/'report.html').write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>psizam and Cranelift: performance report</title><style>body{font:16px/1.5 system-ui;color:#19364b;background:#edf2f5;margin:0}section,details{max-width:980px;background:white;margin:24px auto;padding:40px}details{padding:20px 40px}summary{cursor:pointer;font-weight:600}a{color:#267ca4}img{width:100%}table{border-collapse:collapse;width:100%}td{padding:6px;border-bottom:1px solid #ddd}</style>'+navigation+''.join(web)+'</section>')
    (out/'charts.json').write_text(json.dumps(charts,indent=2)+'\n')
