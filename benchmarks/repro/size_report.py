"""Artifact and native-region size charts, independently of timed execution."""
import json
from pathlib import Path
from report import gm
from run import sha

MODES = ['jit1', 'jit2-pzam', 'llvm-o2-pzam', 'cranelift-aot']
LABELS = ['JIT1 PZAM', 'JIT2 PZAM', 'LLVM O2 PZAM', 'Cranelift AOT']
POLICIES = [('none', 'No CPU limits', '#8397A6'), ('gas', 'Work budget', '#C27C30'),
            ('timeout', 'Time limit', '#267CA4')]


def summarize(work):
    work = Path(work)
    path = work/'artifact-sizes.json'
    if not path.exists(): path = work/'artifact-sizes-jit1.json'
    if not path.exists(): path = work/'artifact-sizes-main.json'
    data = json.loads(path.read_text())
    main = json.loads((work/'resources.json').read_text())
    if data['source_manifest_sha256'] != main['source_manifest_sha256']:
        raise ValueError('Artifact size source identity differs')
    # Bind each saved file measurement to the exact output recorded during
    # compilation, including the last randomized Clang build of each variant.
    expected = {}
    for scope, source in [('.', work), ('coremark', work/'coremark')]:
        if not (source/'resources.json').exists(): continue
        for row in json.loads((source/'resources.json').read_text())['compilation']:
            expected[(scope, row['workload'], row['mode'], row['threads'], row['profile'])] = row['artifact_sha256']
    clang_config = json.loads((work/'clang-resource-config.json').read_text())
    for row in json.loads((work/'clang-resource-compilation.json').read_text()):
        expected[('.', 'clang', row['mode'], row['threads'], row['profile'])] = row['artifact_sha256']
    provenance = data['jit1_provenance']
    jit1 = work/provenance['directory']
    if sha(jit1/'source-manifest.json') != provenance['source_manifest_sha256']:
        raise ValueError('JIT1 writer source identity differs')
    from provenance import verified_manifest
    verified_manifest(jit1, provenance['source_manifest_sha256'])
    # A reproduction kit deliberately omits native binaries. Its explicit
    # packaging record binds the inspected evidence; present files must always
    # match, while ordinary measurement directories require every artifact.
    packaged = work/'packaged-evidence.json'
    omitted = set()
    if packaged.exists():
        record = json.loads(packaged.read_text())
        if record['size_evidence_sha256'] != sha(path):
            raise ValueError('Packaged size evidence changed')
        omitted = set(record['omitted_native_artifacts'])
    cells = {}
    for row in data['rows']:
        row['non_native_payload_bytes'] = row['artifact_bytes'] - row['native_payload_bytes']
        key = (row['scope'], row['workload'], row['mode'], row['threads'], row['profile'])
        artifact = work/row['artifact']
        if not artifact.exists() and row['artifact'] not in omitted:
            raise ValueError('Missing artifact: '+str(key))
        if artifact.exists() and sha(artifact) != row['artifact_sha256']:
            raise ValueError('Artifact bytes changed: '+str(key))
        if row['mode'] != 'jit1' and expected.get(key) != row['artifact_sha256']:
            raise ValueError('Size measurement is not the timed artifact: '+str(key))
        chosen_workers = clang_config['runtime_workers'] if row['workload'] == 'clang' and row['mode'] in ['llvm-o2-pzam','cranelift-aot'] else 1
        if row['threads'] != chosen_workers: continue
        cell = '/'.join([row['workload'], row['mode'], row['profile']])
        if cell in cells: raise ValueError('Duplicate headline size cell')
        cells[cell] = row
    workloads = main['workloads'] + ['clang']
    if any(r['scope'] == 'coremark' for r in data['rows']): workloads.append('coremark')
    for name in workloads:
        for mode in MODES:
            for profile, _, _ in POLICIES:
                key = '/'.join([name, mode, profile])
                if key not in cells and profile == 'timeout' and mode != 'cranelift-aot':
                    cells[key] = dict(cells['/'.join([name, mode, 'none'])], reused_unmetered_code=True)
                if key not in cells: raise ValueError('Missing size cell: '+key)
    return dict(data, cells=cells, compute_workloads=main['workloads'], workloads=workloads)


def render(data, chartpage, title, para):
    c = data['cells']; names = data['compute_workloads']
    def value(n, m, p, metric): return c[n+'/'+m+'/'+p][metric]
    note = ('Exact saved-file sizes for the experiment’s Wasm inputs. JIT1 uses the separately recorded writer build; the other engines use their timed artifacts. Native regions mean PZAM code blobs or ELF executable sections, including constants, stubs and alignment padding. '
            'Wasmtime aligns its executable section to host pages. These are storage/code-region footprints, not instruction counts or hot instruction-cache working sets.')
    chartpage('native-code-index.png', 'Native-code footprint across compute workloads',
        'For each workload, divide an engine’s emitted native-region size by Cranelift’s size with the same resource setting; then take an equal-weight geometric mean.', LABELS,
        [(label, [100*gm(value(n,m,p,'native_payload_bytes')/value(n,'cranelift-aot',p,'native_payload_bytes') for n in names) for m in MODES], color) for p,label,color in POLICIES],
        'Native-region size index | matching Cranelift = 100 | lower is smaller', note, height=330)
    chartpage('artifact-file-index.png', 'Stored artifact size across compute workloads',
        'Complete .pzam and Cranelift AOT file lengths include code, serialized module data, metadata, relocations and padding. Each workload receives equal weight.', LABELS,
        [(label, [100*gm(value(n,m,p,'artifact_bytes')/value(n,'cranelift-aot',p,'artifact_bytes') for n in names) for m in MODES], color) for p,label,color in POLICIES],
        'Saved-file size index | matching Cranelift = 100 | lower is smaller',
        note, height=330)
    chartpage('code-limit-growth.png', 'Code-region growth from resource checks',
        'Additional native-region space compared with each engine’s own unmetered build, aggregated equally across the eight compute workloads.', LABELS,
        [(label, [100*(gm(value(n,m,p,'native_payload_bytes')/value(n,m,'none','native_payload_bytes') for n in names)-1) for m in MODES], color) for p,label,color in POLICIES[1:]],
        'Additional native-region space, percent | lower is smaller',
        'Zero means the recorded region size is unchanged. psizam page interruption reuses unmetered code. Cranelift time limits use epoch instrumentation; page padding can absorb small changes without enlarging its executable section.', percent=True, height=330)
    for metric,filename,heading,axis in [
        ('artifact_bytes','clang-artifact-size.png','Saving the Clang compiler','Complete saved file'),
        ('native_payload_bytes','clang-native-size.png','Native code generated for Clang','Native code region'),
        ('non_native_payload_bytes','clang-non-native-size.png','Clang file space outside native code','File bytes outside the native region')]:
        chartpage(filename,heading,'The same pinned Clang.wasm is compiled by each engine. These sizes affect artifact transfer, disk/cache capacity and the amount of native code available for mapping.', LABELS,
            [(label,[value('clang',m,p,metric)/2**20 for m in MODES],color) for p,label,color in POLICIES],
            axis+', MiB | lower is smaller',
            'Uses the exact runtime artifacts. Four- and eighteen-worker variants remain in the raw size data; worker counts are not separate runtime or size categories. '+note,height=340)
    if 'coremark' in data['workloads']:
        for metric,filename,heading in [
            ('artifact_bytes','coremark-artifact-size.png','Saved files for mixed integer tasks'),
            ('native_payload_bytes','coremark-code-size.png','Native code for mixed integer tasks')]:
            chartpage(filename,heading,
                'The pinned CoreMark program combines linked lists, matrix arithmetic and input classification. Its compact code footprint complements the compiler-sized Clang workload.',
                LABELS,[(label,[value('coremark',m,p,metric)/1024 for m in MODES],color) for p,label,color in POLICIES],
                ('Complete saved file' if metric=='artifact_bytes' else 'Native code region')+', KiB | lower is smaller',
                note,height=340)
    from visual_report import WORKLOAD_TEXT
    for metric,filename,heading,modes,labels in [
        ('artifact_bytes','artifact-size-workloads.png','Saved file sizes for individual workloads',MODES,LABELS),
        ('native_payload_bytes','native-size-workloads.png','Native-code regions for individual workloads',MODES,LABELS)]:
        chartpage(filename,heading,'Unmetered artifacts provide the size reference for each workload. The data tables also retain gas/fuel and timeout variants.',
            [WORKLOAD_TEXT[n]['short'] for n in names],
            [(label,[value(n,m,'none',metric)/1024 for n in names],color) for m,label,color in zip(modes,labels,['#8397A6','#C27C30','#267CA4','#488A69'])],
            'KiB | lower is smaller',note,height=445)
    title('How to use the code-size results')
    para('Complete artifact size matters for distribution and disk caches. Native-region size matters for mapping and code density. Neither alone predicts runtime: hot functions, cache locality, instruction selection and memory traffic determine the working set of a particular application.')
    para('Engine executables and shared runtime/helper code are outside these artifact figures. Runtime linear-memory allocations are also excluded; serialized initial module data remains part of the complete file. These charts measure saved guest artifacts, not total deployment size or resident memory.')
    para('PZAM entry descriptors and ELF function symbols have different coverage. LLVM PZAM descriptors describe entry wrappers rather than every internal body, so their summed ranges are retained only as diagnostics and are not used to rank engines. The comparisons above use complete native regions.')
    llvm=c['clang/llvm-o2-pzam/none']; jit1=c['clang/jit1/none']
    para(f'<b>Storage and loading target:</b> Unmetered Clang LLVM PZAM contains {llvm["relocations"]:,} relocation entries, versus {jit1["relocations"]:,} for JIT1. Its complete file exceeds its native region by {llvm["non_native_payload_bytes"]/2**20:.2f} MiB. Investigate relocation representation and generation as a separate storage/load-time target. The remainder also includes module data and other metadata; this experiment does not isolate relocation bytes or measure relocation processing time.')
    para('JIT1 sizes come from actual .pzam files produced by the CLI. Its writer source archive and compiler hash are recorded separately because the timed JIT1 runner compiles Wasm in memory. No guest code executes during size collection. File sizes are logical byte lengths, not filesystem allocated blocks. Raw records retain exact file hashes, tool hashes and inspection commands.','Small')
