//! C ABI over wasmtime for psizam's wasmtime backend.
//!
//! The C++ side (`include/psizam/wasmtime_backend.hpp`) owns policy: which
//! proposals are enabled, resource limits, and host-function signatures. This
//! crate only translates that into wasmtime calls. Every proposal is set
//! explicitly so a wasmtime upgrade cannot silently change what is accepted.
//!
//! One store holds one instance. Calls nest across stores on one thread, and a
//! host function may re-enter its own store through the context it is handed.

use std::ffi::{c_char, c_void, CString};
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use wasmtime::*;

mod stack_meter;

// ── configuration ───────────────────────────────────────────────────────────

pub const PZWT_FEATURE_SIMD: u32 = 1 << 0;
pub const PZWT_FEATURE_RELAXED_SIMD: u32 = 1 << 1;
pub const PZWT_FEATURE_BULK_MEMORY: u32 = 1 << 2;
pub const PZWT_FEATURE_REFERENCE_TYPES: u32 = 1 << 3;
pub const PZWT_FEATURE_MULTI_VALUE: u32 = 1 << 4;
pub const PZWT_FEATURE_THREADS: u32 = 1 << 5;
pub const PZWT_FEATURE_TAIL_CALL: u32 = 1 << 6;
pub const PZWT_FEATURE_SIGN_EXTENSION: u32 = 1 << 7;
pub const PZWT_FEATURE_SATURATING_FLOAT_TO_INT: u32 = 1 << 8;

/// Deterministic float profiles. `Performance` is wasmtime's own deterministic
/// behaviour (fused relaxed madd). `Strict` is the WebAssembly deterministic
/// profile (unfused relaxed madd), which the fork's compiler emits when asked
/// (`Config::psizam_unfused_madd`); no module is rewritten for it.
// Numbered as psizam numbers them.
pub const PZWT_FLOAT_STRICT: u32 = 0;
pub const PZWT_FLOAT_PERFORMANCE: u32 = 1;

#[repr(C)]
pub struct PzwtConfig {
    pub features: u32,
    pub float_profile: u32,
    /// Native stack available to wasm, in bytes. Not the consensus stack rule.
    pub max_wasm_stack: u64,
    /// If nonzero, the outermost call into wasm on a thread runs on a
    /// dedicated native stack of this size, so that deep recursion is bounded
    /// by the embedder's own stack accounting and not by the calling thread.
    pub native_stack: u64,
    pub epoch_interruption: u8,
    /// Force memory16-v1/p32 on untagged modules. Declared profiles are still
    /// honored when this is zero. This changes execution, not the guest ABI.
    pub memory16: u8,
    /// If nonzero, instances draw their memory and table from this many
    /// preallocated slots instead of mapping and unmapping them each time. A
    /// slot stays warm for the module that last used it. Instantiation fails,
    /// without running anything, when no slot is free or the module does not
    /// fit one; the embedder then needs an engine without a pool.
    pub pool_instances: u32,
    /// Table elements each pooled slot can hold.
    pub pool_table_elements: u32,
    /// The most wasm frames one backtrace holds. Must be nonzero when
    /// `backtrace` is set; ignored otherwise.
    pub backtrace_max_frames: u32,
    /// If nonzero, wasm backtraces are captured: on request through
    /// `pzwt_context_backtrace`, and whenever a call fails. If zero nothing
    /// is ever captured, so a failing call costs no stack walk.
    pub backtrace: u8,
    /// If nonzero, modules keep their DWARF and each captured frame is
    /// resolved to a source file and line. Requires `backtrace`.
    pub backtrace_details: u8,
    /// If nonzero, a module's DWARF is translated to native debug information
    /// and its compiled code is registered with gdb/lldb through the GDB JIT
    /// interface (`__jit_debug_descriptor`).
    pub debug_info: u8,
}

pub const PZWT_OK: i32 = 0;
/// A wasm trap; `trap_code` says which.
pub const PZWT_TRAP: i32 = 1;
/// A host function reported failure; the C++ side holds the exception.
pub const PZWT_HOST_FAILURE: i32 = 2;
/// Anything else: validation, link, or instantiation errors.
pub const PZWT_ERROR: i32 = 3;

pub const PZWT_TRAP_OTHER: i32 = 0;
pub const PZWT_TRAP_STACK_OVERFLOW: i32 = 1;
pub const PZWT_TRAP_MEMORY_OUT_OF_BOUNDS: i32 = 2;
pub const PZWT_TRAP_TABLE_OUT_OF_BOUNDS: i32 = 3;
pub const PZWT_TRAP_INDIRECT_CALL_TO_NULL: i32 = 4;
pub const PZWT_TRAP_BAD_SIGNATURE: i32 = 5;
pub const PZWT_TRAP_INTEGER_OVERFLOW: i32 = 6;
pub const PZWT_TRAP_INTEGER_DIVISION_BY_ZERO: i32 = 7;
pub const PZWT_TRAP_BAD_CONVERSION_TO_INTEGER: i32 = 8;
pub const PZWT_TRAP_UNREACHABLE: i32 = 9;
pub const PZWT_TRAP_INTERRUPT: i32 = 10;
pub const PZWT_TRAP_HEAP_MISALIGNED: i32 = 11;
pub const PZWT_TRAP_ATOMIC_WAIT_NON_SHARED: i32 = 12;

#[repr(C)]
pub struct PzwtError {
    pub status: i32,
    pub trap_code: i32,
    /// Owned by the shim; release with `pzwt_string_free`. May be null.
    pub message: *mut c_char,
}

fn set_error(out: *mut PzwtError, status: i32, trap_code: i32, message: String) {
    if out.is_null() {
        return;
    }
    let message = CString::new(message.replace('\0', " ")).unwrap_or_default();
    unsafe {
        *out = PzwtError { status, trap_code, message: message.into_raw() };
    }
}

fn trap_code(trap: &Trap) -> i32 {
    match trap {
        Trap::StackOverflow => PZWT_TRAP_STACK_OVERFLOW,
        Trap::MemoryOutOfBounds => PZWT_TRAP_MEMORY_OUT_OF_BOUNDS,
        Trap::TableOutOfBounds => PZWT_TRAP_TABLE_OUT_OF_BOUNDS,
        Trap::IndirectCallToNull => PZWT_TRAP_INDIRECT_CALL_TO_NULL,
        Trap::BadSignature => PZWT_TRAP_BAD_SIGNATURE,
        Trap::IntegerOverflow => PZWT_TRAP_INTEGER_OVERFLOW,
        Trap::IntegerDivisionByZero => PZWT_TRAP_INTEGER_DIVISION_BY_ZERO,
        Trap::BadConversionToInteger => PZWT_TRAP_BAD_CONVERSION_TO_INTEGER,
        Trap::UnreachableCodeReached => PZWT_TRAP_UNREACHABLE,
        Trap::Interrupt => PZWT_TRAP_INTERRUPT,
        Trap::HeapMisaligned => PZWT_TRAP_HEAP_MISALIGNED,
        Trap::AtomicWaitNonSharedMemory => PZWT_TRAP_ATOMIC_WAIT_NON_SHARED,
        _ => PZWT_TRAP_OTHER,
    }
}

/// Marker error carried through wasm frames when a host function fails.
#[derive(Debug)]
struct HostFailure;
impl std::fmt::Display for HostFailure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("host function failed")
    }
}
impl std::error::Error for HostFailure {}

/// The text of an error without the wasm frames wasmtime attaches to it when
/// backtraces are captured, so that it reads the same whether they are or
/// not. Everything else wasmtime says, such as the address of a memory fault,
/// is kept. Frames are reported separately, through `failure_backtrace`.
fn message(error: &Error) -> String {
    // wasmtime adds the backtrace as one context of the error.
    let frames = error.downcast_ref::<WasmBacktrace>().map(|b| b.to_string());
    let parts: Vec<String> = error
        .chain()
        .map(|part| part.to_string())
        .filter(|part| Some(part) != frames.as_ref())
        .collect();
    parts.join(": ")
}

fn report(out: *mut PzwtError, error: &Error) -> i32 {
    if error.downcast_ref::<HostFailure>().is_some() {
        set_error(out, PZWT_HOST_FAILURE, 0, String::new());
        PZWT_HOST_FAILURE
    } else if let Some(trap) = error.downcast_ref::<Trap>() {
        set_error(out, PZWT_TRAP, trap_code(trap), message(error));
        PZWT_TRAP
    } else {
        set_error(out, PZWT_ERROR, 0, message(error));
        PZWT_ERROR
    }
}

/// Bump when what the shim attaches to a module before compiling it changes.
const REWRITE_REVISION: u32 = 2;
/// The C ABI and memory16-v1 runtime contract are part of artifact identity.
const SHIM_ABI_REVISION: u32 = 7;
const MEMORY16_PROFILE_REVISION: u32 = 1;

/// Identifies what this build compiles a module into: the wasmtime version,
/// this shim's rewriting, and the target. Part of any key under which a
/// compiled artifact is stored. A static, NUL-terminated string.
#[no_mangle]
pub extern "C" fn pzwt_identity() -> *const c_char {
    static IDENTITY: std::sync::OnceLock<CString> = std::sync::OnceLock::new();
    IDENTITY
        .get_or_init(|| {
            CString::new(format!(
                "wasmtime 51.0.0 source {}; psizam shim {} ABI {} rewrite {} memory16-v{}; {}-{}",
                env!("PSIZAM_WASMTIME_BUILD_ID"),
                env!("CARGO_PKG_VERSION"),
                SHIM_ABI_REVISION,
                REWRITE_REVISION,
                MEMORY16_PROFILE_REVISION,
                std::env::consts::ARCH,
                std::env::consts::OS
            ))
            .unwrap()
        })
        .as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_string_free(message: *mut c_char) {
    if !message.is_null() {
        drop(CString::from_raw(message));
    }
}

// ── engine ──────────────────────────────────────────────────────────────────

pub struct PzwtEngine {
    engine: Engine,
    epoch_interruption: bool,
    native_stack: usize,
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_engine_new(
    config: *const PzwtConfig,
    error: *mut PzwtError,
) -> *mut PzwtEngine {
    let config = &*config;
    if config.float_profile > PZWT_FLOAT_PERFORMANCE {
        set_error(error, PZWT_ERROR, 0, "unknown float profile".into());
        return std::ptr::null_mut();
    }
    if config.memory16 > 1 {
        set_error(error, PZWT_ERROR, 0, "unknown memory16 override".into());
        return std::ptr::null_mut();
    }
    if config.backtrace > 1 || config.backtrace_details > 1 || config.debug_info > 1 {
        set_error(error, PZWT_ERROR, 0, "unknown debugging option value".into());
        return std::ptr::null_mut();
    }
    let backtrace_frames = if config.backtrace != 0 {
        let Some(frames) = std::num::NonZeroUsize::new(config.backtrace_max_frames as usize) else {
            set_error(error, PZWT_ERROR, 0, "backtrace capture needs a nonzero frame limit".into());
            return std::ptr::null_mut();
        };
        Some(frames)
    } else if config.backtrace_details != 0 {
        set_error(error, PZWT_ERROR, 0, "backtrace details need backtrace capture".into());
        return std::ptr::null_mut();
    } else {
        None
    };
    let has = |bit: u32| config.features & bit != 0;
    let mut c = Config::new();

    // Debugging aids, each stated so that neither wasmtime's defaults nor its
    // WASMTIME_BACKTRACE_DETAILS environment variable decide them. With no
    // frame limit wasmtime captures nothing when a call fails.
    c.wasm_backtrace_max_frames(backtrace_frames);
    c.wasm_backtrace_details(if config.backtrace_details != 0 {
        WasmBacktraceDetails::Enable
    } else {
        WasmBacktraceDetails::Disable
    });
    c.debug_info(config.debug_info != 0);

    // This only supplies a profile for untagged source. The fork preserves
    // each module's declared/resolved profile in its own memory metadata, so
    // ordinary and memory16 instances can share this Engine and Linker.
    c.psizam_memory16(config.memory16 != 0);

    if config.pool_instances != 0 {
        let slots = config.pool_instances;
        let mut pool = PoolingAllocationConfig::new();
        pool.total_core_instances(slots)
            .total_memories(slots)
            .total_tables(slots)
            .total_component_instances(0)
            .total_gc_heaps(0)
            // Any wasm32 memory fits; the store's limiter enforces the real cap.
            .max_memory_size(1usize << 32)
            .table_elements(config.pool_table_elements as usize)
            .max_core_instance_size(4 << 20)
            .max_unused_warm_slots(slots)
            // A freed slot's first bytes are zeroed in place rather than
            // unmapped and faulted back in by the next instance.
            .linear_memory_keep_resident(256 << 10)
            .table_keep_resident(64 << 10);
        c.allocation_strategy(InstanceAllocationStrategy::Pooling(pool));
    }

    // Deterministic floating point: positive canonical NaN from every float
    // operation, and one fixed behaviour for each relaxed SIMD instruction.
    c.cranelift_nan_canonicalization(true);
    c.relaxed_simd_deterministic(true);

    // Every proposal is stated, on or off. Nothing is left to wasmtime's
    // defaults, which change between releases.
    c.wasm_simd(has(PZWT_FEATURE_SIMD));
    c.wasm_relaxed_simd(has(PZWT_FEATURE_RELAXED_SIMD));
    c.wasm_bulk_memory(has(PZWT_FEATURE_BULK_MEMORY));
    c.wasm_features(WasmFeatures::REFERENCE_TYPES, has(PZWT_FEATURE_REFERENCE_TYPES));
    c.wasm_multi_value(has(PZWT_FEATURE_MULTI_VALUE));
    c.wasm_threads(has(PZWT_FEATURE_THREADS));
    c.wasm_tail_call(has(PZWT_FEATURE_TAIL_CALL));
    c.wasm_features(WasmFeatures::SIGN_EXTENSION, has(PZWT_FEATURE_SIGN_EXTENSION));
    c.wasm_features(
        WasmFeatures::SATURATING_FLOAT_TO_INT,
        has(PZWT_FEATURE_SATURATING_FLOAT_TO_INT),
    );
    c.wasm_features(WasmFeatures::MUTABLE_GLOBAL, true);
    c.wasm_features(WasmFeatures::FLOATS, true);
    c.shared_memory(false);
    c.wasm_multi_memory(false);
    c.wasm_memory64(false);
    c.wasm_extended_const(false);
    c.wasm_custom_page_sizes(false);
    c.wasm_wide_arithmetic(true);
    c.wasm_features(WasmFeatures::FUNCTION_REFERENCES, false);
    c.wasm_gc(false);
    c.collector(Collector::Null);
    c.wasm_features(WasmFeatures::EXCEPTIONS, false);
    c.wasm_features(WasmFeatures::LEGACY_EXCEPTIONS, false);
    c.wasm_stack_switching(false);
    c.wasm_shared_everything_threads(false);
    c.wasm_branch_hinting(false);
    c.wasm_compact_imports(false);

    // The compiler enforces psizam's stack budget and selects the float
    // profile; no module is rewritten for either.
    c.psizam_stack_meter(true);
    c.psizam_unfused_madd(config.float_profile == PZWT_FLOAT_STRICT);
    c.epoch_interruption(config.epoch_interruption != 0);
    if config.max_wasm_stack != 0 {
        // wasmtime requires its async stack size, which this engine never
        // uses, to be at least the wasm stack limit.
        c.async_stack_size(config.max_wasm_stack as usize);
        c.max_wasm_stack(config.max_wasm_stack as usize);
    }

    match Engine::new(&c) {
        Ok(engine) => Box::into_raw(Box::new(PzwtEngine {
            engine,
            epoch_interruption: config.epoch_interruption != 0,
            native_stack: config.native_stack as usize,
        })),
        Err(e) => {
            report(error, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_engine_delete(engine: *mut PzwtEngine) {
    if !engine.is_null() {
        drop(Box::from_raw(engine));
    }
}

// ── module ──────────────────────────────────────────────────────────────────

pub struct PzwtModule(Module, Entries);

// ── entry points ────────────────────────────────────────────────────────────
//
// A service is entered through a few exports with fixed names and fixed
// signatures. They are found once, when the module is compiled or loaded:
// their names are looked up, their signatures are checked, and where each
// one is among the module's exports is kept. Entering an instance is then an
// index into its exports and a statically typed call, with nothing looked up
// by name and no type examined.

pub const PZWT_ENTRY_START: u32 = 0;
pub const PZWT_ENTRY_CALLED: u32 = 1;
pub const PZWT_ENTRY_SERVE: u32 = 2;
pub const PZWT_ENTRY_PROCESS_TRANSACTION: u32 = 3;
const ENTRY_COUNT: usize = 4;

/// Name and number of i64 parameters of each entry point, by its number. An
/// entry point takes only i64 parameters and returns nothing.
const ENTRIES: [(&str, usize); ENTRY_COUNT] =
    [("start", 1), ("called", 2), ("serve", 0), ("processTransaction", 0)];

/// Where each entry point is among a module's exports, if the module has it.
type Entries = [Option<ModuleExport>; ENTRY_COUNT];

/// Finds a module's entry points. One that is exported with any other
/// signature than its own, or that is not a function, is an error: an entry
/// point is never called with a signature that was not checked here.
fn entries_of(module: &Module) -> std::result::Result<Entries, String> {
    let mut found: Entries = [None; ENTRY_COUNT];
    for (slot, (name, params)) in found.iter_mut().zip(ENTRIES) {
        let Some(ty) = module.get_export(name) else {
            continue;
        };
        let matches = match &ty {
            ExternType::Func(f) => {
                f.results().len() == 0
                    && f.params().len() == params
                    && f.params().all(|param| matches!(param, ValType::I64))
            }
            _ => false,
        };
        if !matches {
            return Err(format!("export {name} does not have the signature of that entry point"));
        }
        *slot = module.get_export_index(name);
    }
    Ok(found)
}

/// Wraps a compiled or loaded module with its entry points.
unsafe fn module_with_entries(module: Module, error: *mut PzwtError) -> *mut PzwtModule {
    match entries_of(&module) {
        Ok(entries) => Box::into_raw(Box::new(PzwtModule(module, entries))),
        Err(e) => {
            set_error(error, PZWT_ERROR, 0, e);
            std::ptr::null_mut()
        }
    }
}

/// Compiles `wasm`, a binary module, with the stack cost of its functions
/// attached: `costs` for a metered module, nothing for one whose functions
/// cost nothing.
unsafe fn compile(
    engine: &PzwtEngine,
    wasm: &[u8],
    costs: Option<&[u32]>,
    error: *mut PzwtError,
) -> *mut PzwtModule {
    let wasm = match stack_meter::with_costs(wasm, costs) {
        Ok(wasm) => wasm,
        Err(e) => {
            set_error(error, PZWT_ERROR, 0, format!("stack accounting: {e}"));
            return std::ptr::null_mut();
        }
    };
    match Module::new(&engine.engine, &wasm) {
        Ok(m) => module_with_entries(m, error),
        Err(e) => {
            report(error, &e);
            std::ptr::null_mut()
        }
    }
}

/// Assembles WebAssembly text into a binary module and hands it to `sink`.
#[no_mangle]
pub unsafe extern "C" fn pzwt_wat_to_wasm(
    text: *const u8,
    len: usize,
    sink: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    sink_data: *mut c_void,
    error: *mut PzwtError,
) -> i32 {
    match wat::parse_bytes(std::slice::from_raw_parts(text, len)) {
        Ok(bytes) => {
            sink(sink_data, bytes.as_ptr(), bytes.len());
            PZWT_OK
        }
        Err(e) => {
            set_error(error, PZWT_ERROR, 0, e.to_string());
            PZWT_ERROR
        }
    }
}

/// Compiles a module with deterministic stack accounting. `wasm` must be a
/// binary module. `costs` gives the byte cost of each defined function, in
/// order; the instance then follows the budget set with
/// `pzwt_context_set_stack`, and its start function, if it has one, is held
/// back for the host to run once a budget is set.
#[no_mangle]
pub unsafe extern "C" fn pzwt_module_new_metered(
    engine: *const PzwtEngine,
    bytes: *const u8,
    len: usize,
    costs: *const u32,
    costs_len: usize,
    error: *mut PzwtError,
) -> *mut PzwtModule {
    let bytes = std::slice::from_raw_parts(bytes, len);
    let costs: &[u32] = if costs_len == 0 || costs.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(costs, costs_len)
    };
    compile(&*engine, bytes, Some(costs), error)
}

/// Compiles a module, binary or text, whose functions cost no stack budget.
#[no_mangle]
pub unsafe extern "C" fn pzwt_module_new(
    engine: *const PzwtEngine,
    bytes: *const u8,
    len: usize,
    error: *mut PzwtError,
) -> *mut PzwtModule {
    let bytes = std::slice::from_raw_parts(bytes, len);
    match wat::parse_bytes(bytes) {
        Ok(binary) => compile(&*engine, &binary, None, error),
        Err(e) => {
            set_error(error, PZWT_ERROR, 0, e.to_string());
            std::ptr::null_mut()
        }
    }
}

/// Hands the compiled artifact to `sink`. The bytes are only valid during the
/// call. They are specific to this wasmtime version, target and configuration.
#[no_mangle]
pub unsafe extern "C" fn pzwt_module_serialize(
    module: *const PzwtModule,
    sink: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    sink_data: *mut c_void,
    error: *mut PzwtError,
) -> i32 {
    match (*module).0.serialize() {
        Ok(bytes) => {
            sink(sink_data, bytes.as_ptr(), bytes.len());
            PZWT_OK
        }
        Err(e) => report(error, &e),
    }
}

/// Loads an artifact produced by `pzwt_module_serialize`. The caller vouches
/// for the bytes: wasmtime does not validate machine code it is given.
#[no_mangle]
pub unsafe extern "C" fn pzwt_module_deserialize(
    engine: *const PzwtEngine,
    bytes: *const u8,
    len: usize,
    error: *mut PzwtError,
) -> *mut PzwtModule {
    let bytes = std::slice::from_raw_parts(bytes, len);
    match Module::deserialize(&(*engine).engine, bytes) {
        Ok(m) => module_with_entries(m, error),
        Err(e) => {
            report(error, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_module_delete(module: *mut PzwtModule) {
    if !module.is_null() {
        drop(Box::from_raw(module));
    }
}

// ── store ───────────────────────────────────────────────────────────────────

struct Data {
    host: *mut c_void,
    memory: Option<Memory>,
    /// The engine's dedicated-stack size, for calls made through this store.
    native_stack: usize,
    limits: StoreLimits,
    interrupt: Arc<AtomicBool>,
    /// The wasm frames of the most recent call if it failed, innermost first.
    failure_backtrace: Vec<Frame>,
}

/// What every call takes: a live borrow of a store. The store hands one out
/// for top-level calls; a host function receives one for the duration of the
/// call so it can re-enter the same store.
#[repr(transparent)]
pub struct PzwtContext(StoreContextMut<'static, Data>);

pub struct PzwtStore {
    // Declared before `store`: the context borrows it.
    context: Option<PzwtContext>,
    store: Box<Store<Data>>,
    engine: Engine,
}

// ── dedicated native stack ──────────────────────────────────────────────────

/// A mapped stack with an inaccessible page below it.
struct NativeStack {
    map: *mut u8,
    map_len: usize,
    base: *mut u8,
    size: usize,
}

impl NativeStack {
    fn new(size: usize) -> Option<NativeStack> {
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize;
        let size = size.div_ceil(page) * page;
        let map_len = size + page;
        let map = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                map_len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            )
        };
        if map == libc::MAP_FAILED {
            return None;
        }
        let map = map as *mut u8;
        unsafe { libc::mprotect(map as *mut c_void, page, libc::PROT_NONE) };
        Some(NativeStack { map, map_len, base: unsafe { map.add(page) }, size })
    }
}

impl Drop for NativeStack {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.map as *mut c_void, self.map_len) };
    }
}

thread_local! {
    // One stack per thread, kept between calls. `Some` only while no call on
    // this thread is using it.
    static IDLE_STACK: std::cell::RefCell<Option<NativeStack>> = const { std::cell::RefCell::new(None) };
    // How many wasm calls are active on this thread.
    static CALL_DEPTH: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

/// Runs `f`. The outermost call on a thread moves to the dedicated stack;
/// calls nested inside it are already there.
fn on_native_stack<R>(size: usize, f: impl FnOnce() -> R) -> R {
    let depth = CALL_DEPTH.get();
    CALL_DEPTH.set(depth + 1);
    let result = if depth > 0 || size == 0 {
        f()
    } else {
        let stack = IDLE_STACK
            .with(|s| s.borrow_mut().take())
            .filter(|s| s.size >= size)
            .or_else(|| NativeStack::new(size));
        match stack {
            Some(stack) => {
                let result = unsafe { psm::on_stack(stack.base, stack.size, f) };
                IDLE_STACK.with(|s| *s.borrow_mut() = Some(stack));
                result
            }
            None => f(),
        }
    };
    CALL_DEPTH.set(depth);
    result
}

#[repr(C)]
pub struct PzwtLimits {
    pub max_memory_bytes: u64,
    pub max_table_elements: u64,
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_store_new(
    engine: *const PzwtEngine,
    host: *mut c_void,
    limits: *const PzwtLimits,
) -> *mut PzwtStore {
    let engine = &*engine;
    let limits = &*limits;
    let data = Data {
        host,
        memory: None,
        native_stack: 0,
        limits: StoreLimitsBuilder::new()
            .memory_size(limits.max_memory_bytes as usize)
            .table_elements(limits.max_table_elements as usize)
            .instances(1)
            .memories(1)
            .tables(1)
            .trap_on_grow_failure(false)
            .build(),
        interrupt: Arc::new(AtomicBool::new(false)),
        failure_backtrace: Vec::new(),
    };
    let mut store = Box::new(Store::new(&engine.engine, data));
    store.limiter(|d| &mut d.limits);
    if engine.epoch_interruption {
        // How a store is interrupted (pzwt_interrupter_fire): its flag is
        // raised, then the engine's epoch is incremented once. Every tick of
        // an engine reaches every store of that engine; only the store whose
        // flag is raised stops, the rest move their deadline one tick on.
        //
        // One tick is enough because of two orders. The flag is raised
        // BEFORE the tick, so a store that looks because of this tick sees
        // it. And the fork measures `Continue(1)` from the epoch the store
        // read before it called this callback, not from the epoch afterwards
        // (gofractally/wasmtime, `new_epoch` in vm/libcalls.rs): a store that
        // was answering an EARLIER tick, for another store, when its own flag
        // was raised and its own tick arrived, looks again. With stock
        // wasmtime that tick was absorbed and the store ran on for ever
        // (gofractally/psibase#2263). Do not move this to an engine without
        // that change.
        store.set_epoch_deadline(1);
        store.epoch_deadline_callback(|state| {
            if state.data().interrupt.load(Ordering::SeqCst) {
                Ok(UpdateDeadline::Interrupt)
            } else {
                Ok(UpdateDeadline::Continue(1))
            }
        });
    }
    let mut result = Box::new(PzwtStore { context: None, store, engine: engine.engine.clone() });
    result.store.data_mut().native_stack = engine.native_stack;
    let context: StoreContextMut<'_, Data> = result.store.as_context_mut();
    result.context = Some(PzwtContext(std::mem::transmute::<
        StoreContextMut<'_, Data>,
        StoreContextMut<'static, Data>,
    >(context)));
    Box::into_raw(result)
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_store_context(store: *mut PzwtStore) -> *mut PzwtContext {
    (*store).context.as_mut().unwrap()
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_store_delete(store: *mut PzwtStore) {
    if !store.is_null() {
        let mut store = Box::from_raw(store);
        store.context = None;
        drop(store);
    }
}

/// Stops one store from any thread. Outlives the store it was made from.
pub struct PzwtInterrupter {
    flag: Arc<AtomicBool>,
    engine: Engine,
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_store_interrupter(store: *mut PzwtStore) -> *mut PzwtInterrupter {
    let store = &mut *store;
    Box::into_raw(Box::new(PzwtInterrupter {
        flag: store.store.data().interrupt.clone(),
        engine: store.engine.clone(),
    }))
}

/// Safe from any thread. Takes effect at the store's next loop iteration or
/// function entry; a host function in progress runs to completion first.
#[no_mangle]
pub unsafe extern "C" fn pzwt_interrupter_fire(interrupter: *const PzwtInterrupter) {
    let interrupter = &*interrupter;
    // Flag first, then the tick: see where the store's callback is set.
    interrupter.flag.store(true, Ordering::SeqCst);
    interrupter.engine.increment_epoch();
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_interrupter_delete(interrupter: *mut PzwtInterrupter) {
    if !interrupter.is_null() {
        drop(Box::from_raw(interrupter));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_context_host(context: *mut PzwtContext) -> *mut c_void {
    (*context).0.data().host
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_context_memory(
    context: *mut PzwtContext,
    base: *mut *mut u8,
    len: *mut usize,
) {
    let cx = &mut (*context).0;
    match cx.data().memory {
        Some(memory) => {
            *base = memory.data_ptr(&*cx);
            *len = memory.data_size(&*cx);
        }
        None => {
            *base = std::ptr::null_mut();
            *len = 0;
        }
    }
}

/// Complete private memory state for trusted snapshots/restores. Unlike
/// `pzwt_context_memory` and import callbacks, this includes memory16 padding.
/// The fork only reports that storage when the memory's actual saved profile
/// guarantees the padded allocation; the engine's source override is not used
/// to infer it. Callers must not retain the pointer beyond the store's lifetime
/// or access it while Wasm is running.
#[no_mangle]
pub unsafe extern "C" fn pzwt_context_memory_storage(
    context: *mut PzwtContext,
    base: *mut *mut u8,
    len: *mut usize,
) {
    let cx = &mut (*context).0;
    match cx.data().memory {
        Some(memory) => {
            *base = memory.data_ptr(&*cx);
            *len = memory
                .psizam_memory16_data_size(&*cx)
                .unwrap_or_else(|| memory.data_size(&*cx));
        }
        None => {
            *base = std::ptr::null_mut();
            *len = 0;
        }
    }
}

/// Sets the stack bytes available to wasm in this store: what the next call
/// into wasm starts with. A call leaves the value as it found it, however it
/// ends. A module compiled without costs never spends it.
#[no_mangle]
pub unsafe extern "C" fn pzwt_context_set_stack(context: *mut PzwtContext, bytes: u32) {
    let cx = &mut (*context).0;
    cx.psizam_set_stack_remaining(u64::from(bytes));
}

/// Stack bytes still available: during a host function, what remained when
/// wasm called it; otherwise the value last set.
#[no_mangle]
pub unsafe extern "C" fn pzwt_context_stack(context: *mut PzwtContext) -> u32 {
    let cx = &mut (*context).0;
    // The budget is set from a u32 and only ever decreases.
    cx.psizam_stack_remaining() as u32
}

// ── backtraces ──────────────────────────────────────────────────────────────

/// One wasm frame, copied out of a `WasmBacktrace`.
struct Frame {
    func_index: u32,
    func_name: Option<String>,
    module_offset: Option<usize>,
    file: Option<String>,
    line: u32,
    column: u32,
}

/// The frames of a backtrace, innermost first; none if there is no backtrace.
fn frames_of(backtrace: Option<&WasmBacktrace>) -> Vec<Frame> {
    let Some(backtrace) = backtrace else {
        return Vec::new();
    };
    backtrace
        .frames()
        .iter()
        .map(|frame| {
            // DWARF lists inlined functions innermost first, so the first
            // symbol is the source position of the frame's own instruction.
            let symbol = frame.symbols().first();
            Frame {
                func_index: frame.func_index(),
                func_name: frame.func_name().map(str::to_owned),
                module_offset: frame.module_offset(),
                file: symbol.and_then(|s| s.file()).map(str::to_owned),
                line: symbol.and_then(|s| s.line()).unwrap_or(0),
                column: symbol.and_then(|s| s.column()).unwrap_or(0),
            }
        })
        .collect()
}

/// A wasm frame as handed to a `PzwtFrameSink`. The strings are UTF-8, not
/// NUL-terminated, and only valid during the sink call.
#[repr(C)]
pub struct PzwtFrame {
    /// Index of the function in the module's function index space.
    pub func_index: u32,
    /// 1-based source line and column; zero when not known.
    pub line: u32,
    pub column: u32,
    /// Nonzero if `module_offset` is known.
    pub has_module_offset: u8,
    /// Offset of the frame's instruction from the start of the module binary
    /// that was compiled.
    pub module_offset: u64,
    /// The function's name from the module's name section, as written there
    /// (not demangled). Null if the module does not name it.
    pub func_name: *const u8,
    pub func_name_len: usize,
    /// Source file from the module's DWARF. Null unless the engine was made
    /// with `backtrace_details` and the DWARF covers the frame.
    pub file: *const u8,
    pub file_len: usize,
}

pub type PzwtFrameSink = unsafe extern "C" fn(*mut c_void, *const PzwtFrame);

unsafe fn emit_frames(frames: &[Frame], sink: PzwtFrameSink, sink_data: *mut c_void) {
    let text = |s: &Option<String>| match s {
        Some(s) => (s.as_ptr(), s.len()),
        None => (std::ptr::null(), 0),
    };
    for frame in frames {
        let (func_name, func_name_len) = text(&frame.func_name);
        let (file, file_len) = text(&frame.file);
        let out = PzwtFrame {
            func_index: frame.func_index,
            line: frame.line,
            column: frame.column,
            has_module_offset: u8::from(frame.module_offset.is_some()),
            module_offset: frame.module_offset.unwrap_or(0) as u64,
            func_name,
            func_name_len,
            file,
            file_len,
        };
        sink(sink_data, &out);
    }
}

/// Hands `sink` the wasm frames now on this thread's stack that belong to the
/// store behind `context`, innermost first, at most the engine's
/// `backtrace_max_frames`. Meant for a host function, which passes the context
/// it was given. Hands over nothing if the engine was made without
/// `backtrace`, or if no wasm of this store is running.
#[no_mangle]
pub unsafe extern "C" fn pzwt_context_backtrace(
    context: *mut PzwtContext,
    sink: PzwtFrameSink,
    sink_data: *mut c_void,
) {
    let backtrace = WasmBacktrace::capture(&(*context).0);
    emit_frames(&frames_of(Some(&backtrace)), sink, sink_data);
}

/// Hands `sink` the wasm frames that were on the stack when the most recent
/// `pzwt_call` in this store failed: where it trapped, or the host function
/// that reported failure was called from. Innermost first, at most the
/// engine's `backtrace_max_frames`. Every `pzwt_call` clears them when it
/// starts, so this hands over nothing if the most recent call succeeded, if
/// none was made, or if the engine was made without `backtrace`.
#[no_mangle]
pub unsafe extern "C" fn pzwt_context_failure_backtrace(
    context: *mut PzwtContext,
    sink: PzwtFrameSink,
    sink_data: *mut c_void,
) {
    emit_frames(&(*context).0.data().failure_backtrace, sink, sink_data);
}

// ── host functions ──────────────────────────────────────────────────────────

pub const PZWT_I32: u8 = 0;
pub const PZWT_I64: u8 = 1;
pub const PZWT_F32: u8 = 2;
pub const PZWT_F64: u8 = 3;
pub const PZWT_V128: u8 = 4;

fn val_type(code: u8) -> Option<ValType> {
    Some(match code {
        PZWT_I32 => ValType::I32,
        PZWT_I64 => ValType::I64,
        PZWT_F32 => ValType::F32,
        PZWT_F64 => ValType::F64,
        PZWT_V128 => ValType::V128,
        _ => return None,
    })
}

fn type_code(ty: &ValType) -> Option<u8> {
    Some(match ty {
        ValType::I32 => PZWT_I32,
        ValType::I64 => PZWT_I64,
        ValType::F32 => PZWT_F32,
        ValType::F64 => PZWT_F64,
        ValType::V128 => PZWT_V128,
        _ => return None,
    })
}

/// A host function as compiled code enters it. `values` holds the arguments
/// from index 0 and receives the results from index 0. Returns zero on
/// success; nonzero means the C++ side has stored an exception to rethrow.
pub type PzwtHostFn = unsafe extern "C" fn(
    env: *mut c_void,
    context: *mut PzwtContext,
    host: *mut c_void,
    memory: *mut u8,
    memory_len: usize,
    // Actual caller memory profile, independent of the engine override.
    memory16: u8,
    values: *mut ValRaw,
) -> i32;

#[repr(C)]
pub struct PzwtImport {
    pub module: *const u8,
    pub module_len: usize,
    pub name: *const u8,
    pub name_len: usize,
    pub params: *const u8,
    pub params_len: usize,
    pub results: *const u8,
    pub results_len: usize,
    pub callback: PzwtHostFn,
    pub env: *mut c_void,
    /// Nonzero if the function takes pointers into linear memory. Functions
    /// that do not are entered without looking the memory up.
    pub needs_memory: u8,
}

struct Env(*mut c_void);
unsafe impl Send for Env {}
unsafe impl Sync for Env {}

pub struct PzwtLinker(Linker<Data>);

/// Registers the host functions once per engine. Modules link against the
/// result; a module that imports anything else fails to instantiate.
#[no_mangle]
pub unsafe extern "C" fn pzwt_linker_new(
    engine: *const PzwtEngine,
    imports: *const PzwtImport,
    count: usize,
    error: *mut PzwtError,
) -> *mut PzwtLinker {
    let engine = &(*engine).engine;
    let mut linker = Linker::<Data>::new(engine);
    for import in std::slice::from_raw_parts(imports, count) {
        let text = |ptr, len| std::str::from_utf8(std::slice::from_raw_parts(ptr, len));
        let (Ok(module), Ok(name)) =
            (text(import.module, import.module_len), text(import.name, import.name_len))
        else {
            set_error(error, PZWT_ERROR, 0, "import name is not UTF-8".into());
            return std::ptr::null_mut();
        };
        let types = |ptr, len| -> Option<Vec<ValType>> {
            std::slice::from_raw_parts(ptr as *const u8, len).iter().map(|c| val_type(*c)).collect()
        };
        let (Some(params), Some(results)) = (
            types(import.params, import.params_len),
            types(import.results, import.results_len),
        ) else {
            set_error(error, PZWT_ERROR, 0, format!("{module}.{name}: unknown value type"));
            return std::ptr::null_mut();
        };
        let ty = FuncType::new(engine, params, results);
        let callback = import.callback;
        let env = Env(import.env);
        let needs_memory = import.needs_memory != 0;
        let defined = linker.func_new_unchecked(
            module,
            name,
            ty,
            move |mut caller: Caller<'_, Data>, values: &mut [MaybeUninit<ValRaw>]| {
                let env = &env;
                let (host, memory) = {
                    let data = caller.data();
                    (data.host, data.memory)
                };
                // A start function can call a host before Data.memory has
                // been cached, and its memory need not be exported. Resolve
                // memory16 from the actual caller instance, never the source
                // override or a previously cached store memory's profile.
                // The same holds for an ordinary module: its memory is the
                // caller's memory0 whether or not it is exported.
                let actual = if needs_memory { caller.psizam_memory0() } else { None };
                let (memory, actual_memory16) = match actual {
                    Some((memory, memory16)) => (Some(memory), memory16),
                    None => (memory, false),
                };
                let (base, len, memory16) = match memory {
                    Some(m) if needs_memory => (
                        m.data_ptr(&caller),
                        m.data_size(&caller),
                        u8::from(actual_memory16),
                    ),
                    _ => (std::ptr::null_mut(), 0, 0),
                };
                let mut context = caller.as_context_mut();
                let status = callback(
                    env.0,
                    &mut context as *mut StoreContextMut<'_, Data> as *mut PzwtContext,
                    host,
                    base,
                    len,
                    memory16,
                    values.as_mut_ptr() as *mut ValRaw,
                );
                if status == 0 {
                    Ok(())
                } else {
                    Err(Error::new(HostFailure))
                }
            },
        );
        if let Err(e) = defined {
            report(error, &e);
            return std::ptr::null_mut();
        }
    }
    Box::into_raw(Box::new(PzwtLinker(linker)))
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_linker_delete(linker: *mut PzwtLinker) {
    if !linker.is_null() {
        drop(Box::from_raw(linker));
    }
}

// ── instance ────────────────────────────────────────────────────────────────

/// An instance, where its module's entry points are, and the function of
/// each entry point that has been entered: resolved from the export's index
/// the first time, since a function handle belongs to one instance.
pub struct PzwtInstance(Instance, Entries, std::cell::Cell<[Option<Func>; ENTRY_COUNT]>);

/// Instantiates `module` in the store behind `context`. Data and element
/// segments are applied; a start function, if the module has one, runs.
#[no_mangle]
pub unsafe extern "C" fn pzwt_instantiate(
    context: *mut PzwtContext,
    linker: *const PzwtLinker,
    module: *const PzwtModule,
    error: *mut PzwtError,
) -> *mut PzwtInstance {
    let cx = &mut (*context).0;
    let native_stack = cx.data().native_stack;
    let outcome =
        on_native_stack(native_stack, || (*linker).0.instantiate(&mut *cx, &(*module).0));
    match outcome {
        Ok(instance) => {
            // Host functions take pointers into the module's memory whether
            // or not the module exports it, as psizam's engines allow.
            let memory = instance
                .get_memory(&mut *cx, "memory")
                .or_else(|| instance.psizam_memory0(&*cx).map(|(memory, _)| memory));
            cx.data_mut().memory = memory;
            Box::into_raw(Box::new(PzwtInstance(
                instance,
                (*module).1,
                std::cell::Cell::new([None; ENTRY_COUNT]),
            )))
        }
        Err(e) => {
            report(error, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_instance_delete(instance: *mut PzwtInstance) {
    if !instance.is_null() {
        drop(Box::from_raw(instance));
    }
}

pub struct PzwtFunc(Func);

/// Resolves an exported function and reports its signature so the caller can
/// check it once. `params`/`results` receive up to `capacity` type codes each.
/// Returns null if there is no such export or it uses an unsupported type.
#[no_mangle]
pub unsafe extern "C" fn pzwt_instance_func(
    context: *mut PzwtContext,
    instance: *const PzwtInstance,
    name: *const u8,
    name_len: usize,
    params: *mut u8,
    params_len: *mut usize,
    results: *mut u8,
    results_len: *mut usize,
    capacity: usize,
) -> *mut PzwtFunc {
    let cx = &mut (*context).0;
    let Ok(name) = std::str::from_utf8(std::slice::from_raw_parts(name, name_len)) else {
        return std::ptr::null_mut();
    };
    // A metered module's start function is held back by the engine, exported
    // or not; the host reaches it under a reserved name.
    let func = if name == stack_meter::START {
        (*instance).0.psizam_deferred_start(&mut *cx)
    } else {
        (*instance).0.get_func(&mut *cx, name)
    };
    let Some(func) = func else {
        return std::ptr::null_mut();
    };
    let ty = func.ty(&*cx);
    let write = |types: Vec<ValType>, out: *mut u8, out_len: *mut usize| -> bool {
        if types.len() > capacity {
            return false;
        }
        for (i, t) in types.iter().enumerate() {
            match type_code(t) {
                Some(code) => *out.add(i) = code,
                None => return false,
            }
        }
        *out_len = types.len();
        true
    };
    if !write(ty.params().collect(), params, params_len)
        || !write(ty.results().collect(), results, results_len)
    {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(PzwtFunc(func)))
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_func_delete(func: *mut PzwtFunc) {
    if !func.is_null() {
        drop(Box::from_raw(func));
    }
}

/// Reports how a call into wasm ended.
unsafe fn finish_call(
    cx: &mut StoreContextMut<'static, Data>,
    outcome: Result<()>,
    error: *mut PzwtError,
) -> i32 {
    match outcome {
        Ok(()) => PZWT_OK,
        Err(e) => {
            cx.data_mut().failure_backtrace = frames_of(e.downcast_ref::<WasmBacktrace>());
            // An exhausted budget is wasmtime's own stack-overflow trap, as is
            // an exhausted native stack; the native stack is sized so that the
            // budget always runs out first.
            let overflowed = matches!(e.downcast_ref::<Trap>(), Some(Trap::StackOverflow));
            if overflowed {
                set_error(error, PZWT_TRAP, PZWT_TRAP_STACK_OVERFLOW, "stack overflow".into());
                PZWT_TRAP
            } else {
                report(error, &e)
            }
        }
    }
}

/// The status `pzwt_call_entry` returns when the module does not export the
/// entry point. Nothing ran.
pub const PZWT_NO_ENTRY: i32 = 3;

/// Enters `instance` through entry point number `entry` (`PZWT_ENTRY_*`) with
/// that entry point's arguments: `a` for `start`; `a` and `b` for `called`;
/// neither is read for the others. No type is examined: the signature was
/// checked when the module was compiled or loaded.
#[no_mangle]
pub unsafe extern "C" fn pzwt_call_entry(
    context: *mut PzwtContext,
    instance: *const PzwtInstance,
    entry: u32,
    a: u64,
    b: u64,
    error: *mut PzwtError,
) -> i32 {
    let cx = &mut (*context).0;
    let instance = &*instance;
    if entry as usize >= ENTRY_COUNT {
        return PZWT_NO_ENTRY;
    }
    let mut funcs = instance.2.get();
    let func = match funcs[entry as usize] {
        Some(func) => func,
        None => {
            let Some(export) = instance.1[entry as usize].as_ref() else {
                return PZWT_NO_ENTRY;
            };
            let Some(Extern::Func(func)) = instance.0.get_module_export(&mut *cx, export) else {
                return PZWT_NO_ENTRY;
            };
            funcs[entry as usize] = Some(func);
            instance.2.set(funcs);
            func
        }
    };
    let native_stack = cx.data().native_stack;
    cx.data_mut().failure_backtrace.clear();
    // The arguments go straight into the two slots the call reads; an entry
    // point returns nothing. This is wasmtime's cheapest way into wasm: its
    // statically typed call measured nearly twice as slow.
    // SAFETY: `entries_of` checked this export against exactly this signature,
    // which reads no more than two i64 slots and writes none.
    let mut values = [ValRaw::i64(a as i64), ValRaw::i64(b as i64)];
    let outcome = on_native_stack(native_stack, || {
        func.call_unchecked(&mut *cx, std::ptr::slice_from_raw_parts_mut(values.as_mut_ptr(), 2))
    });
    finish_call(cx, outcome, error)
}

/// Whether the module `instance` was made from exports entry point `entry`.
#[no_mangle]
pub unsafe extern "C" fn pzwt_instance_has_entry(instance: *const PzwtInstance, entry: u32) -> u8 {
    u8::from((*instance).1.get(entry as usize).is_some_and(|e| e.is_some()))
}

/// Calls an export. `values` holds the arguments from index 0 and receives the
/// results from index 0; `count` must cover whichever is longer. Types are not
/// checked here: the caller checked them against `pzwt_instance_func`.
#[no_mangle]
pub unsafe extern "C" fn pzwt_call(
    context: *mut PzwtContext,
    func: *const PzwtFunc,
    values: *mut ValRaw,
    count: usize,
    error: *mut PzwtError,
) -> i32 {
    let cx = &mut (*context).0;
    let values = std::ptr::slice_from_raw_parts_mut(values, count);
    let native_stack = cx.data().native_stack;
    let func = &(*func).0;
    cx.data_mut().failure_backtrace.clear();
    let outcome = on_native_stack(native_stack, || func.call_unchecked(&mut *cx, values));
    finish_call(cx, outcome, error)
}
