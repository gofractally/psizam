//! Deterministic stack accounting, injected into the module.
//!
//! wasmtime limits the native stack, and how much native stack a wasm frame
//! takes depends on the compiler. psizam instead charges each function a fixed
//! number of bytes computed from the wasm itself. This pass makes any engine
//! follow that rule: it adds one mutable i32 global holding the bytes that
//! remain, and wraps every defined function so that it
//!
//!   - traps if fewer bytes remain than the function costs,
//!   - subtracts its cost on entry,
//!   - adds it back on every normal exit.
//!
//! The costs come from the caller (psizam's validator computes them). The
//! global is exported under `STACK_EXPORT` so the host can set the budget
//! before a call and read what is left during one.
//!
//! That is `Placement::Rewrite`. With `Placement::Compiler` the same rule is
//! enforced by code the compiler emits (wasmtime's `psizam_stack_meter`), and
//! none of the above is done: `with_costs` only appends the costs to the
//! module in a custom section, after everything else, so the module's code is
//! byte for byte what it was and its debug information still describes it.
//! The engine holds back the start function and keeps the budget itself.

use wasm_encoder::reencode::{Error, Reencode};
use wasm_encoder::{
    BlockType, CodeSection, ConstExpr, ExportKind, ExportSection, GlobalSection, GlobalType,
    Instruction, Module, SectionId, ValType,
};
use wasmparser::{Operator, Parser, Payload};

pub const STACK_EXPORT: &str = "psizam:stack-remaining";

/// The module's start function is not run at instantiation, where no budget
/// has been set yet. It is exported under this name for the host to call.
pub const START_EXPORT: &str = "psizam:start";

/// Host functions take pointers into the module's memory whether or not the
/// module exports it, as psizam's engines allow. The memory is exported under
/// this name so the host can always reach it.
pub const MEMORY_EXPORT: &str = "psizam:memory";

/// Written to the counter just before the overflow trap, so the host can tell
/// a stack overflow from an ordinary `unreachable`. Budgets stay below 2^31.
pub const OVERFLOW_SENTINEL: u32 = u32::MAX;

/// Where the stack accounting of a metered module is done.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Placement {
    /// In the module: a counter global and a wrapper around every function.
    Rewrite,
    /// In compiled code, from costs carried in `COSTS_SECTION`. Every module
    /// compiled by such an engine carries the section, metered or not.
    Compiler,
}

/// The custom section a `Placement::Compiler` module carries its costs in:
/// one little-endian `u32` per defined function. The compiler reads the last
/// section of this name, so one already in the module has no effect.
pub const COSTS_SECTION: &str = "psizam.stack-costs";

/// The empty custom section by which a metered `Placement::Compiler` module
/// has its start function held back for the host to call.
pub const DEFER_START_SECTION: &str = "psizam.defer-start";

/// Returns `wasm` with the stack cost of each of its defined functions
/// appended for the compiler. Without `costs` every function costs nothing,
/// which is what an unmetered module is on an engine that meters.
pub fn with_costs(wasm: &[u8], costs: Option<&[u32]>) -> Result<Vec<u8>, String> {
    let metered = costs.is_some();
    let zeros;
    let costs = match costs {
        Some(costs) => costs,
        None => {
            let mut functions = 0usize;
            for payload in Parser::new(0).parse_all(wasm) {
                if let Payload::FunctionSection(section) = payload.map_err(|e| e.to_string())? {
                    functions += section.count() as usize;
                }
            }
            zeros = vec![0u32; functions];
            &zeros
        }
    };
    if costs.iter().any(|cost| *cost >= 1 << 31) {
        return Err("function stack cost is too large".to_string());
    }
    let data: Vec<u8> = costs.iter().flat_map(|cost| cost.to_le_bytes()).collect();
    let mut out = wasm.to_vec();
    let section = wasm_encoder::CustomSection { name: COSTS_SECTION.into(), data: data.into() };
    wasm_encoder::Section::append_to(&section, &mut out);
    if metered {
        // The start function needs a budget, which the host sets after
        // instantiation: the engine holds it back until the host calls it.
        let section = wasm_encoder::CustomSection {
            name: DEFER_START_SECTION.into(),
            data: (&[][..]).into(),
        };
        wasm_encoder::Section::append_to(&section, &mut out);
    }
    Ok(out)
}

struct Meter<'a> {
    /// Per-function byte costs, or `None` to leave stack use unmetered.
    costs: Option<&'a [u32]>,
    /// Replace relaxed multiply-add with a separate multiply and add.
    unfuse_madd: bool,
    /// Parameter count of each defined function, in code-section order.
    params: Vec<u32>,
    /// Result types of each defined function, in code-section order.
    results: Vec<Vec<wasmparser::ValType>>,
    stack_global: u32,
    next_function: usize,
    has_global_section: bool,
    has_export_section: bool,
    added_global: bool,
    added_export: bool,
    /// The module's start function, if it has one.
    start: Option<u32>,
    /// Whether the module has a memory (index 0).
    has_memory: bool,
    /// Types to append: `() -> results` for functions with several results,
    /// which the wrapping block needs and the module may not declare.
    extra_types: Vec<Vec<wasmparser::ValType>>,
    /// Index of the `() -> results` type for each defined function that needs one.
    block_types: Vec<Option<u32>>,
}

/// Position of a section in a module. Custom sections have none.
fn rank(id: SectionId) -> Option<u32> {
    Some(match id {
        SectionId::Type => 10,
        SectionId::Import => 20,
        SectionId::Function => 30,
        SectionId::Table => 40,
        SectionId::Memory => 50,
        SectionId::Tag => 55,
        SectionId::Global => 60,
        SectionId::Export => 70,
        SectionId::Start => 80,
        SectionId::Element => 90,
        SectionId::DataCount => 95,
        SectionId::Code => 100,
        SectionId::Data => 110,
        _ => return None,
    })
}

impl Meter<'_> {
    fn add_exports(&mut self, exports: &mut ExportSection) {
        exports.export(STACK_EXPORT, ExportKind::Global, self.stack_global);
        if let Some(start) = self.start {
            exports.export(START_EXPORT, ExportKind::Func, start);
        }
        if self.has_memory {
            exports.export(MEMORY_EXPORT, ExportKind::Memory, 0);
        }
        self.added_export = true;
    }

    fn counter_global(&self) -> (GlobalType, ConstExpr) {
        (
            GlobalType { val_type: ValType::I32, mutable: true, shared: false },
            ConstExpr::i32_const(0),
        )
    }
}

impl Reencode for Meter<'_> {
    type Error = String;

    fn parse_type_section(
        &mut self,
        types: &mut wasm_encoder::TypeSection,
        section: wasmparser::TypeSectionReader<'_>,
    ) -> Result<(), Error<String>> {
        wasm_encoder::reencode::utils::parse_type_section(self, types, section)?;
        for results in std::mem::take(&mut self.extra_types) {
            let results = results
                .into_iter()
                .map(|t| self.val_type(t))
                .collect::<Result<Vec<_>, _>>()?;
            types.ty().function([], results);
        }
        Ok(())
    }

    fn parse_global_section(
        &mut self,
        globals: &mut GlobalSection,
        section: wasmparser::GlobalSectionReader<'_>,
    ) -> Result<(), Error<String>> {
        wasm_encoder::reencode::utils::parse_global_section(self, globals, section)?;
        if self.costs.is_some() {
            let (ty, init) = self.counter_global();
            globals.global(ty, &init);
            self.added_global = true;
        }
        Ok(())
    }

    fn parse_export_section(
        &mut self,
        exports: &mut ExportSection,
        section: wasmparser::ExportSectionReader<'_>,
    ) -> Result<(), Error<String>> {
        wasm_encoder::reencode::utils::parse_export_section(self, exports, section)?;
        if self.costs.is_some() {
            self.add_exports(exports);
        }
        Ok(())
    }

    fn intersperse_section_hook(
        &mut self,
        module: &mut Module,
        _after: Option<SectionId>,
        before: Option<SectionId>,
    ) -> Result<(), Error<String>> {
        if self.costs.is_none() {
            return Ok(());
        }
        // A module with no global or export section still needs the counter:
        // add the missing section where it belongs.
        let next = match before {
            Some(id) => match rank(id) {
                Some(r) => r,
                None => return Ok(()),
            },
            None => u32::MAX,
        };
        if !self.has_global_section && !self.added_global && next > 60 {
            let mut globals = GlobalSection::new();
            let (ty, init) = self.counter_global();
            globals.global(ty, &init);
            module.section(&globals);
            self.added_global = true;
        }
        if !self.has_export_section && !self.added_export && next > 70 {
            let mut exports = ExportSection::new();
            self.add_exports(&mut exports);
            module.section(&exports);
        }
        Ok(())
    }

    fn parse_function_body(
        &mut self,
        code: &mut CodeSection,
        func: wasmparser::FunctionBody<'_>,
    ) -> Result<(), Error<String>> {
        let index = self.next_function;
        self.next_function += 1;

        // Locals, plus two v128 scratch locals if a multiply-add is rewritten.
        let mut locals = Vec::new();
        let mut local_count = *self.params.get(index).unwrap_or(&0);
        for entry in func.get_locals_reader()? {
            let (count, ty) = entry?;
            local_count = local_count
                .checked_add(count)
                .ok_or_else(|| Error::UserError("too many locals".to_string()))?;
            locals.push((count, self.val_type(ty)?));
        }
        let mut scratch = None;
        if self.unfuse_madd {
            let mut reader = func.get_operators_reader()?;
            while !reader.eof() {
                if matches!(
                    reader.read()?,
                    Operator::F32x4RelaxedMadd
                        | Operator::F32x4RelaxedNmadd
                        | Operator::F64x2RelaxedMadd
                        | Operator::F64x2RelaxedNmadd
                ) {
                    scratch = Some(local_count);
                    locals.push((2, ValType::V128));
                    break;
                }
            }
        }
        let mut f = wasm_encoder::Function::new(locals);

        let metered = match self.costs {
            Some(costs) => {
                let cost = *costs.get(index).ok_or_else(|| {
                    Error::UserError("fewer stack costs than functions".to_string())
                })?;
                if cost >= 1 << 31 {
                    return Err(Error::UserError("function stack cost is too large".to_string()));
                }
                Some(cost as i32)
            }
            None => None,
        };
        let g = self.stack_global;
        if let Some(cost) = metered {
            let results = self.results.get(index).cloned().unwrap_or_default();
            let block = match results.as_slice() {
                [] => BlockType::Empty,
                [one] => BlockType::Result(self.val_type(*one)?),
                _ => BlockType::FunctionType(self.block_types[index].ok_or_else(|| {
                    Error::UserError("missing block type for a multi-result function".to_string())
                })?),
            };
            // if remaining < cost: mark the counter and trap
            f.instruction(&Instruction::GlobalGet(g));
            f.instruction(&Instruction::I32Const(cost));
            f.instruction(&Instruction::I32LtU);
            f.instruction(&Instruction::If(BlockType::Empty));
            f.instruction(&Instruction::I32Const(OVERFLOW_SENTINEL as i32));
            f.instruction(&Instruction::GlobalSet(g));
            f.instruction(&Instruction::Unreachable);
            f.instruction(&Instruction::End);
            // remaining -= cost
            f.instruction(&Instruction::GlobalGet(g));
            f.instruction(&Instruction::I32Const(cost));
            f.instruction(&Instruction::I32Sub);
            f.instruction(&Instruction::GlobalSet(g));
            // The original body runs inside one block, so that every way of
            // leaving it normally arrives at the code that gives the bytes
            // back. A branch to the function's own label already lands there,
            // because the block takes that label's place; `return` is turned
            // into one.
            f.instruction(&Instruction::Block(block));
        }

        let mut reader = func.get_operators_reader()?;
        let mut depth: u32 = 0;
        let mut closed = false;
        while !reader.eof() {
            let op = reader.read()?;
            match op {
                Operator::Block { .. }
                | Operator::Loop { .. }
                | Operator::If { .. }
                | Operator::TryTable { .. }
                | Operator::Try { .. } => depth += 1,
                Operator::End if depth == 0 => {
                    if let Some(cost) = metered {
                        // End of the function: close the block, give the bytes back.
                        f.instruction(&Instruction::End);
                        f.instruction(&Instruction::GlobalGet(g));
                        f.instruction(&Instruction::I32Const(cost));
                        f.instruction(&Instruction::I32Add);
                        f.instruction(&Instruction::GlobalSet(g));
                    }
                    f.instruction(&Instruction::End);
                    closed = true;
                    continue;
                }
                Operator::End => depth -= 1,
                Operator::Return if metered.is_some() => {
                    f.instruction(&Instruction::Br(depth));
                    continue;
                }
                Operator::ReturnCall { .. }
                | Operator::ReturnCallIndirect { .. }
                | Operator::ReturnCallRef { .. }
                    if metered.is_some() =>
                {
                    return Err(Error::UserError(
                        "stack accounting does not support tail calls".to_string(),
                    ))
                }
                // relaxed_madd(a, b, c) = a*b + c and relaxed_nmadd = (-a)*b + c,
                // each as two separately rounded operations. The operands
                // arrive as a, b, c with c on top.
                Operator::F32x4RelaxedMadd | Operator::F64x2RelaxedMadd if scratch.is_some() => {
                    let t = scratch.unwrap();
                    let wide = matches!(op, Operator::F64x2RelaxedMadd);
                    f.instruction(&Instruction::LocalSet(t));
                    f.instruction(if wide { &Instruction::F64x2Mul } else { &Instruction::F32x4Mul });
                    f.instruction(&Instruction::LocalGet(t));
                    f.instruction(if wide { &Instruction::F64x2Add } else { &Instruction::F32x4Add });
                    continue;
                }
                Operator::F32x4RelaxedNmadd | Operator::F64x2RelaxedNmadd if scratch.is_some() => {
                    let t = scratch.unwrap();
                    let wide = matches!(op, Operator::F64x2RelaxedNmadd);
                    f.instruction(&Instruction::LocalSet(t));
                    f.instruction(&Instruction::LocalSet(t + 1));
                    f.instruction(if wide { &Instruction::F64x2Neg } else { &Instruction::F32x4Neg });
                    f.instruction(&Instruction::LocalGet(t + 1));
                    f.instruction(if wide { &Instruction::F64x2Mul } else { &Instruction::F32x4Mul });
                    f.instruction(&Instruction::LocalGet(t));
                    f.instruction(if wide { &Instruction::F64x2Add } else { &Instruction::F32x4Add });
                    continue;
                }
                _ => {}
            }
            f.instruction(&self.instruction(op)?);
        }
        if !closed {
            return Err(Error::UserError("function body has no end".to_string()));
        }
        code.function(&f);
        Ok(())
    }
}

/// Removes the start section, returning the module without it and the start
/// function's index. Sections are `id, size (LEB128), contents`.
fn strip_start(wasm: &[u8]) -> Result<(Vec<u8>, Option<u32>), String> {
    fn leb(bytes: &[u8], at: &mut usize) -> Result<u32, String> {
        let mut value = 0u32;
        for shift in (0..35).step_by(7) {
            let byte = *bytes.get(*at).ok_or("truncated module")?;
            *at += 1;
            value |= u32::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return Ok(value);
            }
        }
        Err("invalid integer in module".to_string())
    }
    let mut out = wasm.get(..8).ok_or("truncated module")?.to_vec();
    let mut start = None;
    let mut at = 8;
    while at < wasm.len() {
        let begin = at;
        let id = wasm[at];
        at += 1;
        let size = leb(wasm, &mut at)? as usize;
        let end = at.checked_add(size).filter(|e| *e <= wasm.len()).ok_or("truncated section")?;
        if id == 8 {
            let mut inner = at;
            start = Some(leb(wasm, &mut inner)?);
        } else {
            out.extend_from_slice(&wasm[begin..end]);
        }
        at = end;
    }
    Ok((out, start))
}

/// Returns `wasm` rewritten. With `costs` (one entry per defined function, in
/// order) stack accounting is added. With `unfuse_madd`, relaxed multiply-add
/// becomes a multiply followed by an add, which is the WebAssembly
/// deterministic profile's definition; without it the engine's own (fused)
/// behaviour stands.
pub fn rewrite(wasm: &[u8], costs: Option<&[u32]>, unfuse_madd: bool) -> Result<Vec<u8>, String> {
    let (stripped, start) = if costs.is_some() {
        strip_start(wasm)?
    } else {
        (wasm.to_vec(), None)
    };
    let wasm = stripped.as_slice();
    // (parameter count, results) of every type, by index.
    let mut signatures: Vec<(usize, Vec<wasmparser::ValType>)> = Vec::new();
    let mut types: Vec<Vec<wasmparser::ValType>> = Vec::new();
    let mut function_types: Vec<u32> = Vec::new();
    let mut globals: u32 = 0;
    let mut has_global_section = false;
    let mut has_export_section = false;
    let mut has_memory = false;

    for payload in Parser::new(0).parse_all(wasm) {
        match payload.map_err(|e| e.to_string())? {
            Payload::TypeSection(section) => {
                for group in section {
                    for ty in group.map_err(|e| e.to_string())?.into_types() {
                        let (params, results) = match &ty.composite_type.inner {
                            wasmparser::CompositeInnerType::Func(f) => {
                                (f.params().len(), f.results().to_vec())
                            }
                            _ => (usize::MAX, Vec::new()),
                        };
                        signatures.push((params, results.clone()));
                        types.push(results);
                    }
                }
            }
            Payload::ImportSection(section) => {
                for import in section.into_imports() {
                    match import.map_err(|e| e.to_string())?.ty {
                        wasmparser::TypeRef::Global(_) => globals += 1,
                        wasmparser::TypeRef::Memory(_) => has_memory = true,
                        _ => {}
                    }
                }
            }
            Payload::FunctionSection(section) => {
                for ty in section {
                    function_types.push(ty.map_err(|e| e.to_string())?);
                }
            }
            Payload::MemorySection(section) => has_memory |= section.count() > 0,
            Payload::GlobalSection(section) => {
                has_global_section = true;
                globals += section.count();
            }
            Payload::ExportSection(section) => {
                has_export_section = true;
                for export in section {
                    let name = export.map_err(|e| e.to_string())?.name;
                    if [STACK_EXPORT, START_EXPORT, MEMORY_EXPORT].contains(&name) {
                        return Err(format!("module exports the reserved name {name}"));
                    }
                }
            }
            _ => {}
        }
    }
    if let Some(costs) = costs {
        if costs.len() != function_types.len() {
            return Err(format!(
                "{} stack costs for {} functions",
                costs.len(),
                function_types.len()
            ));
        }
    }
    let params = function_types
        .iter()
        .map(|t| signatures.get(*t as usize).map_or(0, |(params, _)| *params as u32))
        .collect();
    let results: Vec<Vec<wasmparser::ValType>> = function_types
        .iter()
        .map(|t| types.get(*t as usize).cloned().unwrap_or_default())
        .collect();
    // A function with several results is wrapped in a block of type
    // `() -> results`. Use a type the module already has, or add one.
    let mut extra_types: Vec<Vec<wasmparser::ValType>> = Vec::new();
    let block_types = results
        .iter()
        .map(|r| {
            if r.len() < 2 || costs.is_none() {
                return None;
            }
            if let Some(i) = signatures.iter().position(|(params, res)| *params == 0 && res == r) {
                return Some(i as u32);
            }
            let extra = extra_types.iter().position(|e| e == r).unwrap_or_else(|| {
                extra_types.push(r.clone());
                extra_types.len() - 1
            });
            Some((signatures.len() + extra) as u32)
        })
        .collect();

    let mut meter = Meter {
        costs,
        unfuse_madd,
        params,
        results,
        stack_global: globals,
        next_function: 0,
        has_global_section,
        has_export_section,
        added_global: false,
        added_export: false,
        start,
        has_memory,
        extra_types,
        block_types,
    };
    let mut module = Module::new();
    meter
        .parse_core_module(&mut module, Parser::new(0), wasm)
        .map_err(|e| e.to_string())?;
    if costs.is_some() && (!meter.added_global || !meter.added_export) {
        return Err("could not place the stack counter in the module".to_string());
    }
    Ok(module.finish())
}
