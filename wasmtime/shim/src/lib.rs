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
/// profile (unfused relaxed madd) and needs the psizam fork of wasmtime.
pub const PZWT_FLOAT_PERFORMANCE: u32 = 0;
pub const PZWT_FLOAT_STRICT: u32 = 1;

#[repr(C)]
pub struct PzwtConfig {
    pub features: u32,
    pub float_profile: u32,
    /// Native stack available to wasm, in bytes. Not the consensus stack rule.
    pub max_wasm_stack: u64,
    pub epoch_interruption: u8,
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

fn report(out: *mut PzwtError, error: &Error) -> i32 {
    if error.downcast_ref::<HostFailure>().is_some() {
        set_error(out, PZWT_HOST_FAILURE, 0, String::new());
        PZWT_HOST_FAILURE
    } else if let Some(trap) = error.downcast_ref::<Trap>() {
        set_error(out, PZWT_TRAP, trap_code(trap), format!("{error:#}"));
        PZWT_TRAP
    } else {
        set_error(out, PZWT_ERROR, 0, format!("{error:#}"));
        PZWT_ERROR
    }
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
}

#[no_mangle]
pub unsafe extern "C" fn pzwt_engine_new(
    config: *const PzwtConfig,
    error: *mut PzwtError,
) -> *mut PzwtEngine {
    let config = &*config;
    if config.float_profile != PZWT_FLOAT_PERFORMANCE {
        set_error(
            error,
            PZWT_ERROR,
            0,
            "the strict float profile (unfused relaxed madd) needs the psizam wasmtime fork".into(),
        );
        return std::ptr::null_mut();
    }
    let has = |bit: u32| config.features & bit != 0;
    let mut c = Config::new();

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
    c.wasm_wide_arithmetic(false);
    c.wasm_features(WasmFeatures::FUNCTION_REFERENCES, false);
    c.wasm_gc(false);
    c.wasm_features(WasmFeatures::EXCEPTIONS, false);
    c.wasm_features(WasmFeatures::LEGACY_EXCEPTIONS, false);
    c.wasm_stack_switching(false);
    c.wasm_shared_everything_threads(false);
    c.wasm_branch_hinting(false);
    c.wasm_compact_imports(false);

    c.epoch_interruption(config.epoch_interruption != 0);
    if config.max_wasm_stack != 0 {
        c.max_wasm_stack(config.max_wasm_stack as usize);
    }

    match Engine::new(&c) {
        Ok(engine) => Box::into_raw(Box::new(PzwtEngine {
            engine,
            epoch_interruption: config.epoch_interruption != 0,
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

pub struct PzwtModule(Module);

#[no_mangle]
pub unsafe extern "C" fn pzwt_module_new(
    engine: *const PzwtEngine,
    bytes: *const u8,
    len: usize,
    error: *mut PzwtError,
) -> *mut PzwtModule {
    let bytes = std::slice::from_raw_parts(bytes, len);
    match Module::new(&(*engine).engine, bytes) {
        Ok(m) => Box::into_raw(Box::new(PzwtModule(m))),
        Err(e) => {
            report(error, &e);
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
        Ok(m) => Box::into_raw(Box::new(PzwtModule(m))),
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
    limits: StoreLimits,
    interrupt: Arc<AtomicBool>,
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
        limits: StoreLimitsBuilder::new()
            .memory_size(limits.max_memory_bytes as usize)
            .table_elements(limits.max_table_elements as usize)
            .instances(1)
            .memories(1)
            .tables(1)
            .trap_on_grow_failure(false)
            .build(),
        interrupt: Arc::new(AtomicBool::new(false)),
    };
    let mut store = Box::new(Store::new(&engine.engine, data));
    store.limiter(|d| &mut d.limits);
    if engine.epoch_interruption {
        // Every engine tick reaches every store. Only the store whose flag was
        // raised stops; the rest move their deadline one tick on.
        store.set_epoch_deadline(1);
        store.epoch_deadline_callback(|state| {
            if state.data().interrupt.load(Ordering::Relaxed) {
                Ok(UpdateDeadline::Interrupt)
            } else {
                Ok(UpdateDeadline::Continue(1))
            }
        });
    }
    let mut result = Box::new(PzwtStore { context: None, store, engine: engine.engine.clone() });
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
    interrupter.flag.store(true, Ordering::Relaxed);
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
                let (base, len) = match memory {
                    Some(m) if needs_memory => (m.data_ptr(&caller), m.data_size(&caller)),
                    _ => (std::ptr::null_mut(), 0),
                };
                let mut context = caller.as_context_mut();
                let status = callback(
                    env.0,
                    &mut context as *mut StoreContextMut<'_, Data> as *mut PzwtContext,
                    host,
                    base,
                    len,
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

pub struct PzwtInstance(Instance);

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
    match (*linker).0.instantiate(&mut *cx, &(*module).0) {
        Ok(instance) => {
            let memory = instance.get_memory(&mut *cx, "memory");
            cx.data_mut().memory = memory;
            Box::into_raw(Box::new(PzwtInstance(instance)))
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
    let Some(func) = (*instance).0.get_func(&mut *cx, name) else {
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
    match (*func).0.call_unchecked(&mut *cx, values) {
        Ok(()) => PZWT_OK,
        Err(e) => report(error, &e),
    }
}
