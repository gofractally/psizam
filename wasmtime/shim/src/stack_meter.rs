//! Deterministic stack accounting.
//!
//! wasmtime limits the native stack, and how much native stack a wasm frame
//! takes depends on the compiler. psizam instead charges each function a fixed
//! number of bytes computed from the wasm itself, so that every engine fails
//! the same call for a given budget.
//!
//! The fork's compiler enforces that rule (`Config::psizam_stack_meter`): the
//! budget travels as a hidden argument, a function traps with a stack overflow
//! if less remains than it costs and passes the difference to what it calls,
//! and the store keeps the value across the boundary with the host. What the
//! compiler needs from the embedder is the cost of each function, which
//! psizam's validator computes. `with_costs` attaches them to the module in a
//! custom section.
//!
//! Nothing else about the module changes. The sections are appended after
//! everything the module contains, so its code is byte for byte what was
//! deployed and its debug information still describes it.

use wasm_encoder::{CustomSection, Section};
use wasmparser::{Parser, Payload};

/// The custom section a module carries its costs in: one little-endian `u32`
/// per defined function, in order. The compiler reads the last section of
/// this name, so one already in the module has no effect.
pub const COSTS_SECTION: &str = "psizam.stack-costs";

/// The empty custom section by which a metered module has its start function
/// held back. A start function costs stack like any other, and the host can
/// only set a budget once the instance exists; it then runs the function
/// itself, under `START`.
pub const DEFER_START_SECTION: &str = "psizam.defer-start";

/// The name under which the host asks an instance for its held-back start
/// function. It is not an export: a module cannot define it.
pub const START: &str = "psizam:start";

/// Returns `wasm` with the stack cost of each of its defined functions
/// attached. Without `costs` every function costs nothing and the start
/// function runs at instantiation as usual: an unmetered module.
pub fn with_costs(wasm: &[u8], costs: Option<&[u32]>) -> Result<Vec<u8>, String> {
    let mut functions = 0usize;
    for payload in Parser::new(0).parse_all(wasm) {
        if let Payload::FunctionSection(section) = payload.map_err(|e| e.to_string())? {
            functions += section.count() as usize;
        }
    }
    let metered = costs.is_some();
    let zeros;
    let costs = match costs {
        Some(costs) => costs,
        None => {
            zeros = vec![0u32; functions];
            &zeros
        }
    };
    if costs.len() != functions {
        return Err(format!("{} stack costs for {} functions", costs.len(), functions));
    }
    // The host sets budgets as u32 and reports a spent budget as an i32-sized
    // quantity elsewhere; a cost beyond this can never be met.
    if costs.iter().any(|cost| *cost >= 1 << 31) {
        return Err("function stack cost is too large".to_string());
    }
    let data: Vec<u8> = costs.iter().flat_map(|cost| cost.to_le_bytes()).collect();
    let mut out = wasm.to_vec();
    CustomSection { name: COSTS_SECTION.into(), data: data.into() }.append_to(&mut out);
    if metered {
        CustomSection { name: DEFER_START_SECTION.into(), data: (&[][..]).into() }
            .append_to(&mut out);
    }
    Ok(out)
}
