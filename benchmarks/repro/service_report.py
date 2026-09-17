"""Validated summaries and charts for independently protected service calls."""
import json
import statistics

MODES = ['jit1', 'jit2', 'llvm', 'llvm-aot', 'cranelift-checked', 'cranelift-cached']
LABELS = ['JIT1', 'JIT2', 'LLVM O2 JIT', 'LLVM O2 PZAM', 'Cranelift checked', 'Cranelift cached']
POLICIES = [('none', 'No CPU limits', '#8397A6'), ('gas', 'Work budget', '#C27C30'),
            ('timeout', 'Time limit', '#267CA4')]


def summarize(work, manifest):
    result = {}
    expected_source = {k:v for k,v in manifest['psizam'].items() if k.startswith(('include/', 'src/'))}
    for directory, fields, expected in [
        ('service-calls', ['mode', 'policy'], {(m,p) for m in MODES for p,_,_ in POLICIES}),
        ('runtime-calls', ['mode', 'binding'], {(m,b) for m in ['jit1','jit2','llvm'] for b in ['typed','dynamic']})]:
        config = json.loads((work/directory/'config.json').read_text())
        rows = json.loads((work/directory/'measurements.json').read_text())
        if config['source_sha256'] != expected_source:
            raise ValueError(directory+': engine source identity differs')
        if any(not r.get('valid') for r in rows): raise ValueError(directory+': invalid result')
        if {tuple(r[f] for f in fields) for r in rows} != expected:
            raise ValueError(directory+': incomplete matrix')
        cells = {}
        for key in sorted(expected):
            group = [r for r in rows if tuple(r[f] for f in fields)==key]
            measured = [r for r in group if r['sample']>=0]
            if sorted(r['sample'] for r in group) != list(range(-1,config['samples'])):
                raise ValueError(directory+': wrong trial count')
            metrics = [k for k in measured[0] if k.endswith(('_ns','_allocations','_gas')) or k=='ns_per_call']
            cells['/'.join(key)] = {k:statistics.median(r[k] for r in measured) for k in metrics}
            cells['/'.join(key)]['ranges'] = {k:[min(r[k] for r in measured),max(r[k] for r in measured)] for k in metrics}
            if directory=='runtime-calls' and any(not r['consumer_gas'] or not r['provider_gas'] for r in group):
                raise ValueError('Runtime proxy gas instrumentation missing')
        result[directory] = {'config':config, 'cells':cells, 'trials':len(rows)}
    return result


def render(data, chartpage, title, para):
    natural = data['service-calls']['cells']
    for metric, filename, heading, description, unit in [
        ('repeated_host_query_ns','service-queries.png','Frequent database-style host queries',
         'A guest loop issues two distinct scalar host queries per iteration. Results feed subsequent operations and are checked against a native reference. This isolates dispatch overhead paid repeatedly by database-heavy services.', 'Nanoseconds per query, including loop instructions'),
        ('service_a_host_service_b_ns','service-roundtrip.png','Calling another service through the host',
         'A host enters service A, A calls the host to invoke B, and B makes a host query before returning through A. Each guest entry has its own trap boundary. Linking and compilation are outside timing.', 'Nanoseconds per complete A → host → B round trip'),
        ('service_a_b_a_ns','service-reentry.png','Nested service calls that reenter the caller',
         'The call chain enters A, dispatches through the host into B, then reenters A. Three independently protected guest entries exercise nested execution and state restoration.', 'Nanoseconds per complete A → B → A round trip')]:
        chartpage(filename, heading, description, LABELS,
            [(label,[natural[m+'/'+p][metric] for m in MODES],color) for p,label,color in POLICIES],
            unit+' | lower is better',
            'Five measured fresh processes after one excluded warmup per cell. Cranelift uses Wasmtime C APIs with cached exports; its cached mode validates the signature at binding and uses raw slots, retaining a trap boundary per invocation. '
            'psizam uses checked indexed calls in this natural-signature test. These tiny scalar payloads do not measure a database, serialization, or full application IPC.', height=390)
    runtime = data['runtime-calls']['cells']
    allocations=max(cell['ranges']['cpp_allocations'][1] for cell in runtime.values())
    allocation_note=('All measured psizam proxy trials allocate zero times through C++ new/new[]. ' if allocations==0 else f'The largest observed C++ new/new[] count is {allocations:,.0f} per million calls. ')
    chartpage('runtime-bindings.png','Actual C++ runtime proxies between services',
        'The host invokes instance::as, the consumer calls a provider through runtime::bind, and the provider returns the scalar sum. Both typed C++ binding and WIT-driven dynamic binding use the canonical calling convention.',
        ['JIT1','JIT2','LLVM O2 JIT'],
        [('Typed binding',[runtime[m+'/typed']['ns_per_call'] for m in ['jit1','jit2','llvm']],'#267CA4'),
         ('Dynamic binding',[runtime[m+'/dynamic']['ns_per_call'] for m in ['jit1','jit2','llvm']],'#C27C30')],
        'Nanoseconds per host → consumer → provider call | lower is better',
        'One million scalar calls per trial, five measured trials and one warmup. Compilation and linking are excluded. Both backends have active gas instrumentation and a 10^12 budget; actual gas consumption is verified. '
        +allocation_note+'This is a separate ABI experiment, with no corresponding Wasmtime component binding, and must not be read as an engine-to-engine comparison.',height=280)
    title('What the service measurements establish')
    para('<b>Cache stable work at binding:</b> Reflected proxies and scalar bindings retain validated export indices and callable adapters. Inline canonical argument slots avoid dynamic C++ allocation. Large guarded entry stacks are reused per instance and nesting depth; active reentries never share a stack.')
    para('<b>Keep protection at every boundary:</b> Each cached call retains trap recovery, call-depth accounting and resource enforcement. LLVM imports use one exception-safe helper for dispatch, depth accounting and memory refresh. Signal-mask restoration is paid when escaping a signal, rather than on every successful entry.')
    para('<b>Remaining performance targets:</b> Compare the protected natural-signature entries with cached Cranelift, then isolate argument adaptation and trap setup. JIT1/JIT2 imports still repack arguments and dispatch through a generic host adapter. Measure changes on the query and nested-service cases before rerunning the compute and compiler matrix.')
    para('<b>Remaining application coverage:</b> Add cold-linking costs, variable-size records and strings, realistic database work, and full service dispatch. Dynamic non-scalar WIT marshalling and logical schema compatibility need separate acceptance tests. A lowered sixteen-i64 signature alone does not prove logical WIT compatibility. These experiments establish scalar boundary costs, not production IPC latency.','Small')
