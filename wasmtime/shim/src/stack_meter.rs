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

use wasm_encoder::reencode::{Error, Reencode};
use wasm_encoder::{
    BlockType, CodeSection, ConstExpr, ExportKind, ExportSection, GlobalSection, GlobalType,
    Instruction, Module, SectionId, ValType,
};
use wasmparser::{Operator, Parser, Payload};

pub const STACK_EXPORT: &str = "psizam:stack-remaining";

/// Written to the counter just before the overflow trap, so the host can tell
/// a stack overflow from an ordinary `unreachable`. Budgets stay below 2^31.
pub const OVERFLOW_SENTINEL: u32 = u32::MAX;

struct Meter<'a> {
    costs: &'a [u32],
    /// Result types of each defined function, in code-section order.
    results: Vec<Vec<wasmparser::ValType>>,
    stack_global: u32,
    next_function: usize,
    has_global_section: bool,
    has_export_section: bool,
    added_global: bool,
    added_export: bool,
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
    fn counter_global(&self) -> (GlobalType, ConstExpr) {
        (
            GlobalType { val_type: ValType::I32, mutable: true, shared: false },
            ConstExpr::i32_const(0),
        )
    }
}

impl Reencode for Meter<'_> {
    type Error = String;

    fn parse_global_section(
        &mut self,
        globals: &mut GlobalSection,
        section: wasmparser::GlobalSectionReader<'_>,
    ) -> Result<(), Error<String>> {
        wasm_encoder::reencode::utils::parse_global_section(self, globals, section)?;
        let (ty, init) = self.counter_global();
        globals.global(ty, &init);
        self.added_global = true;
        Ok(())
    }

    fn parse_export_section(
        &mut self,
        exports: &mut ExportSection,
        section: wasmparser::ExportSectionReader<'_>,
    ) -> Result<(), Error<String>> {
        wasm_encoder::reencode::utils::parse_export_section(self, exports, section)?;
        exports.export(STACK_EXPORT, ExportKind::Global, self.stack_global);
        self.added_export = true;
        Ok(())
    }

    fn intersperse_section_hook(
        &mut self,
        module: &mut Module,
        _after: Option<SectionId>,
        before: Option<SectionId>,
    ) -> Result<(), Error<String>> {
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
            exports.export(STACK_EXPORT, ExportKind::Global, self.stack_global);
            module.section(&exports);
            self.added_export = true;
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
        let cost = *self
            .costs
            .get(index)
            .ok_or_else(|| Error::UserError("fewer stack costs than functions".to_string()))?;
        if cost >= 1 << 31 {
            return Err(Error::UserError("function stack cost is too large".to_string()));
        }
        let cost = cost as i32;
        let results = self.results.get(index).cloned().unwrap_or_default();
        let block = match results.as_slice() {
            [] => BlockType::Empty,
            [one] => BlockType::Result(self.val_type(*one)?),
            _ => {
                return Err(Error::UserError(
                    "stack accounting does not support multiple results".to_string(),
                ))
            }
        };
        let g = self.stack_global;

        let mut f = self.new_function_with_parsed_locals(&func)?;
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
        // leaving it normally arrives at the code that gives the bytes back.
        // A branch to the function's own label already lands there, because
        // the block takes that label's place; `return` is turned into one.
        f.instruction(&Instruction::Block(block));

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
                    // End of the function: close the block, give the bytes back.
                    f.instruction(&Instruction::End);
                    f.instruction(&Instruction::GlobalGet(g));
                    f.instruction(&Instruction::I32Const(cost));
                    f.instruction(&Instruction::I32Add);
                    f.instruction(&Instruction::GlobalSet(g));
                    f.instruction(&Instruction::End);
                    closed = true;
                    continue;
                }
                Operator::End => depth -= 1,
                Operator::Return => {
                    f.instruction(&Instruction::Br(depth));
                    continue;
                }
                Operator::ReturnCall { .. }
                | Operator::ReturnCallIndirect { .. }
                | Operator::ReturnCallRef { .. } => {
                    return Err(Error::UserError(
                        "stack accounting does not support tail calls".to_string(),
                    ))
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

/// Returns `wasm` with stack accounting added. `costs` has one entry per
/// defined function, in order.
pub fn add_stack_accounting(wasm: &[u8], costs: &[u32]) -> Result<Vec<u8>, String> {
    let mut types: Vec<Vec<wasmparser::ValType>> = Vec::new();
    let mut function_types: Vec<u32> = Vec::new();
    let mut globals: u32 = 0;
    let mut has_global_section = false;
    let mut has_export_section = false;

    for payload in Parser::new(0).parse_all(wasm) {
        match payload.map_err(|e| e.to_string())? {
            Payload::TypeSection(section) => {
                for group in section {
                    for ty in group.map_err(|e| e.to_string())?.into_types() {
                        types.push(match &ty.composite_type.inner {
                            wasmparser::CompositeInnerType::Func(f) => f.results().to_vec(),
                            _ => Vec::new(),
                        });
                    }
                }
            }
            Payload::ImportSection(section) => {
                for import in section.into_imports() {
                    if let wasmparser::TypeRef::Global(_) = import.map_err(|e| e.to_string())?.ty {
                        globals += 1;
                    }
                }
            }
            Payload::FunctionSection(section) => {
                for ty in section {
                    function_types.push(ty.map_err(|e| e.to_string())?);
                }
            }
            Payload::GlobalSection(section) => {
                has_global_section = true;
                globals += section.count();
            }
            Payload::ExportSection(section) => {
                has_export_section = true;
                for export in section {
                    if export.map_err(|e| e.to_string())?.name == STACK_EXPORT {
                        return Err(format!("module already exports {STACK_EXPORT}"));
                    }
                }
            }
            _ => {}
        }
    }
    if costs.len() != function_types.len() {
        return Err(format!(
            "{} stack costs for {} functions",
            costs.len(),
            function_types.len()
        ));
    }
    let results = function_types
        .iter()
        .map(|t| types.get(*t as usize).cloned().unwrap_or_default())
        .collect();

    let mut meter = Meter {
        costs,
        results,
        stack_global: globals,
        next_function: 0,
        has_global_section,
        has_export_section,
        added_global: false,
        added_export: false,
    };
    let mut module = Module::new();
    meter
        .parse_core_module(&mut module, Parser::new(0), wasm)
        .map_err(|e| e.to_string())?;
    if !meter.added_global || !meter.added_export {
        return Err("could not place the stack counter in the module".to_string());
    }
    Ok(module.finish())
}
