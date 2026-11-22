#pragma once

// LLVM AOT codegen writer — subclass of ir_writer that translates IR to LLVM IR,
// then compiles to a relocatable object file via LLVM TargetMachine (AOT).
//
// Per-function compilation: each function gets its own LLVM context/module,
// is translated, optimized, and emitted independently. This bounds peak memory
// to module_metadata + one_function_IR + one_LLVM_module, which is critical
// for wasm32-hosted compilation where address space is limited to 4GB.
//
// Cross-function calls become code_blob_self relocations with negative addends
// encoding the target function index. After all functions are compiled, these
// are resolved to actual code blob offsets.

#include <psizam/detail/ir_writer.hpp>
#include <psizam/detail/llvm_ir_translator.hpp>
#include <psizam/detail/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace psizam::detail {

   class ir_writer_llvm_aot : public ir_writer {
    public:
      static constexpr bool compile_import_only_module = true;
      // LLVM embedders build their own map from the linked artifact.
      static constexpr bool debug_map_from_function_bounds = false;
      ir_writer_llvm_aot(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                         bool enable_backtrace = false, bool stack_limit_is_bytes = false,
                         uint32_t compile_threads = 0)
         : ir_writer_impl(alloc, source_bytes, mod, enable_backtrace, stack_limit_is_bytes, compile_threads)
      {
         _skip_codegen = true;
      }

      // Shadow base class: compile each function via LLVM immediately after parsing.
      // Creates a fresh LLVM module per function, translates, optimizes, emits,
      // then discards — bounding LLVM memory to one function at a time.
      void finalize(function_body& body) {
#if !defined(__wasi__)
         if (_compile_threads > 1) {
            // Parallel mode: skip serial compilation, will be done in destructor
            ir_writer_impl::finalize(body);
            _ir_alloc._offset = _post_array_offset;
            return;
         }
#endif
         if (_compile_result && _func && !_had_error) {
            compile_current_function();
         }
         // Base class clears _func and resets IR allocator (since _skip_codegen,
         // base finalize just clears _func — IR reset done here)
         ir_writer_impl::finalize(body);
         // Reset IR allocator to reclaim this function's IR data
         _ir_alloc._offset = _post_array_offset;
      }

      ~ir_writer_llvm_aot() noexcept {
#ifdef __EXCEPTIONS
         if (std::uncaught_exceptions() > 0) return;
#endif
         if (!_compile_result) return;
         if (_had_error) return;

         if (_compile_result->target_triple.empty()) {
            _compile_result->error = "ir_writer_llvm_aot: target_triple not set on compile_result";
            return;
         }

#if !defined(__wasi__)
         if (_num_functions && _compile_threads > 1 && _parse_callback) {
            parallel_llvm_compile_all();
            return;
         }
#endif

         if (!_num_functions && _mod.get_imported_functions_size()) {
            compile_current_function(true);
            if (_had_error) return;
         }

         resolve_cross_function_relocs();
         if (_had_error) return;
         finish_native_frames();
         if (_had_error) return;

         _compile_result->relocs = std::move(_accumulated_relocs);
         _compile_result->code_blob = std::move(_accumulated_code);
         _compile_result->function_offsets = std::move(_function_offsets);
         _compile_result->body_offsets = _func_body_offsets;
      }

    private:

      void record_import_thunks(const auto& result, uint32_t base, bool owner) {
         const auto count = _mod.get_imported_functions_size();
         if (result.native_guest_abi != psizam::native_guest_abi::llvm_tailcc ||
             result.import_thunk_offsets.size() != count || result.import_thunk_stack_sizes.size() != count ||
             (owner && _import_unit_present)) {
            _compile_result->error = "invalid LLVM typed import unit";
            _had_error = true;
            return;
         }
         if (owner) {
            _import_thunk_offsets.resize(count);
            _import_thunk_stack_sizes.resize(count);
         }
         for (uint32_t i = 0; i < count; ++i) {
            const auto [offset, size] = result.import_thunk_offsets[i];
            if ((!owner && size) || (owner && (!size || result.import_thunk_stack_sizes[i] == UINT64_MAX)) ||
                uint64_t(base) + offset + size > uint64_t(INT32_MAX)) {
               _compile_result->error = "incomplete or duplicate LLVM typed import coverage";
               _had_error = true;
               return;
            }
            if (owner) {
               _import_thunk_offsets[i] = {base + offset, size};
               _import_thunk_stack_sizes[i] = result.import_thunk_stack_sizes[i];
            }
         }
         if (owner) _import_unit_present = true;
      }

      void record_native_frames(const auto& result, uint32_t index) {
         if (!result.native_stack_frames_complete || result.native_stack_probe_size != 4096 ||
             result.entry_stack_sizes.size() != _num_functions ||
             result.body_stack_sizes.size() != _num_functions || index >= _num_functions) {
            _compile_result->error = "incomplete LLVM native frame result";
            _had_error = true;
            return;
         }
         _entry_stack_sizes.resize(_num_functions);
         _body_stack_sizes.resize(_num_functions);
         _native_frame_presence.resize(_num_functions);
         _entry_stack_sizes[index] = result.entry_stack_sizes[index];
         _body_stack_sizes[index] = result.body_stack_sizes[index];
         _native_frame_presence[index] = 1;
      }

      void finish_native_frames() {
         if (_mod.get_imported_functions_size() && !_import_unit_present) {
            _compile_result->error = "missing LLVM typed import unit";
            _had_error = true;
            return;
         }
         for (uint32_t index = 0; index < _num_functions; ++index) {
            if (index >= _native_frame_presence.size() || !_native_frame_presence[index] ||
                index >= _function_offsets.size() || !_function_offsets[index].second ||
                index >= _func_body_offsets.size() || !_func_body_offsets[index].second) {
               _compile_result->error = "incomplete module native frame coverage";
               _had_error = true;
               return;
            }
         }
         for (uint32_t index = 0; index < _num_functions; ++index) {
            _mod.code[index].native_stack_frame_bytes = _body_stack_sizes[index];
            _mod.code[index].native_stack_frame_complete = true;
         }
         _compile_result->entry_stack_sizes = std::move(_entry_stack_sizes);
         _compile_result->body_stack_sizes = std::move(_body_stack_sizes);
         _compile_result->native_stack_probe_size = 4096;
         _compile_result->native_stack_frames_complete = true;
         _compile_result->native_guest_abi = psizam::native_guest_abi::llvm_tailcc;
         _mod.native_guest_abi = psizam::native_guest_abi::llvm_tailcc;
         _compile_result->import_thunk_offsets = std::move(_import_thunk_offsets);
         _compile_result->import_thunk_stack_sizes = std::move(_import_thunk_stack_sizes);
      }

      void resolve_cross_function_relocs() {
         // Resolve pending cross-function relocations.
         // Negative addends encode function indices:
         //   Body refs:  addend = -(code_idx + 1)              [range -1..-N]
         //   Entry refs: addend = -(code_idx + 1 + code_count) [range -(N+1)..-(2N)]
         uint32_t code_count = static_cast<uint32_t>(_function_offsets.size());
         for (auto& r : _accumulated_relocs) {
            if (r.symbol == reloc_symbol::code_blob_self && r.addend < 0) {
               uint64_t neg_val = static_cast<uint64_t>(-int64_t(r.addend));
               uint64_t resolved = UINT64_MAX;
               if (neg_val <= code_count) {
                  // Body ref: -(code_idx + 1), resolve to wasm_func_N offset
                  uint32_t code_idx = neg_val - 1;
                  if (code_idx < _func_body_offsets.size()) {
                     if (_func_body_offsets[code_idx].second)
                        resolved = _func_body_offsets[code_idx].first;
                  }
               } else if (neg_val <= uint64_t{2} * code_count) {
                  // Entry ref: -(code_idx + 1 + code_count), resolve to wasm_entry_N offset
                  uint32_t code_idx = neg_val - 1 - code_count;
                  if (code_idx < _function_offsets.size()) {
                     if (_function_offsets[code_idx].second)
                        resolved = _function_offsets[code_idx].first;
                  }
               } else {
                  const auto import = neg_val - uint64_t{2} * code_count - 1;
                  if (import < _import_thunk_offsets.size() && _import_thunk_offsets[import].second)
                     resolved = _import_thunk_offsets[import].first;
               }
               if (resolved > uint64_t(INT32_MAX)) {
                  _compile_result->error = "unresolved LLVM typed function relocation";
                  _had_error = true;
                  return;
               }
               r.addend = static_cast<int32_t>(resolved);
            }
         }
      }

#if !defined(__wasi__)
      // Batch compilation: group consecutive function indices into batches and
      // compile each batch as a single LLVM module. Amortizes LLVMContext /
      // TargetMachine / PassBuilder / ELF emission cost across many functions.
      //
      // Determinism: batch_i = function indices [i*batch_size, (i+1)*batch_size),
      // a pure function of (input, thread_count). Final code blob lays batches
      // in batch-index order → bit-identical output for a given thread count.
      //
      // NOTE: batch_size depends on _compile_threads, so .pzam output is
      // thread-count-dependent. For consensus use, pin thread count in the
      // compiler identity hash.

      // Minimum batch size — tiny batches lose the amortization benefit of
      // batching (per-batch overhead dominates).
      static constexpr uint32_t k_min_batch_size = 256;

      void parallel_llvm_compile_all() {
         uint32_t N = _compile_threads;
         if (_num_functions == 0) return;

         // Fixed batch size: empirically tuned for best wall-clock balance
         // between amortizing per-batch overhead and keeping enough batches
         // for load balancing across heterogeneous function sizes.
         // Scaling batch_size with N / num_functions (fewer larger batches)
         // was measured to HURT wall time — less load balancing beats the
         // reduced per-batch setup cost.
         const uint32_t batch_size = 1024;
         const uint32_t num_batches = (_num_functions + batch_size - 1) / batch_size;
         if (N > num_batches) N = num_batches;

         // Per-batch results
         struct batch_result_t {
            std::vector<uint8_t> code;
            std::vector<code_relocation> relocations;
            // Sized to _num_functions; only indices in [batch_start, batch_end)
            // are populated for this batch.
            std::vector<std::pair<uint32_t, uint32_t>> function_offsets;
            std::vector<std::pair<uint32_t, uint32_t>> body_offsets;
            std::vector<uint64_t> entry_stack_sizes;
            std::vector<uint64_t> body_stack_sizes;
            std::vector<std::pair<uint32_t, uint32_t>> import_thunk_offsets;
            std::vector<uint64_t> import_thunk_stack_sizes;
            psizam::native_guest_abi native_guest_abi = psizam::native_guest_abi::legacy_caller_cleanup;
            uint32_t native_stack_probe_size = 0;
            bool native_stack_frames_complete = false;
            uint32_t blob_offset = 0;  // set during between phase
         };
         auto batch_results = std::make_unique<batch_result_t[]>(num_batches);

         // Per-worker state
         struct worker_state {
            growable_allocator ir_alloc;
            uint64_t max_stack = 0;
            bool had_error = false;
            std::string error;
         };
         auto workers = std::make_unique<worker_state[]>(N);

         // Build translate options (shared, read-only)
         llvm_translate_options topts;
         topts.opt_level        = _compile_result->opt_level;
         topts.fp               = _compile_result->softfloat ? fp_mode::softfloat
                                                             : fp_mode::hw_deterministic;
         topts.profile          = this->_float_profile;
         // Batch mode still uses ExternalLinkage for all wasm_func_N so that
         // cross-batch refs resolve via the symbol table. Same-batch refs
         // produce negative-addend relocs that resolve_cross_function_relocs
         // later patches to the final merged-blob offsets.
         topts.per_function     = true;
         topts.enable_backtrace = _compile_result->backtrace;
         topts.mem_mode = _mem_mode;
         topts.checked_kind = _checked_kind;
         const std::string& target_triple = _compile_result->target_triple;

         auto& reactor = get_compile_reactor(N);

         // Initialize function/body offset arrays
         _function_offsets.resize(_num_functions, {0, 0});
         _func_body_offsets.resize(_num_functions, {0, 0});

         std::atomic<bool> any_error{false};

         reactor.run_batch(
            num_batches,

            // work_fn: parse + translate + LLVM compile a batch of functions
            [this, &workers, &batch_results, &topts, &target_triple, &any_error,
             batch_size]
            (uint32_t worker_id, uint32_t batch_idx) {
               if (any_error.load(std::memory_order_relaxed)) return;
               auto& ws = workers[worker_id];

               // Lazy init worker IR allocator
               if (!ws.ir_alloc._base) {
#if defined(PSIZAM_COMPILE_ONLY) && !defined(__LP64__)
                  // WASM32: fixed allocation sized for largest function.
                  // Only ONE function's IR is alive at a time (reset between
                  // translations within the batch), so batching does not
                  // increase the IR-alloc footprint.
                  ws.ir_alloc.use_fixed_memory(growable_allocator::align_to_page(
                     _max_func_bytes * 250 + 1024 * 1024));
#else
                  // Native or 64-bit: demand-paged, only commits what's used
                  ws.ir_alloc.use_default_memory();
#endif
               }

               const uint32_t start = batch_idx * batch_size;
               const uint32_t end = std::min(start + batch_size, _num_functions);

               // Translate all functions in the batch into a single LLVM module,
               // then compile once. ELF emission + parse + TargetMachine cost
               // is amortized across `end - start` functions.
               auto do_compile = [&]() {
                  auto local_options = topts;
                  local_options.emit_import_thunks = batch_idx == 0;
                  llvm_ir_translator translator(get_ir_module(), local_options);

                  for (uint32_t func_idx = start; func_idx < end; ++func_idx) {
                     ir_writer_impl worker_writer(_mod, _functions, _num_functions,
                                                 ws.ir_alloc, _enable_backtrace,
                                                 _stack_limit_is_bytes);
                     _parse_callback(func_idx, worker_writer, ws.max_stack);

                     translator.translate_function(_functions[func_idx]);

                     // Release this function's IR — LLVM IR is in the module now
                     ws.ir_alloc._offset = 0;
                  }

                  translator.finalize();

                  auto llvm_mod = translator.take_module();
                  auto llvm_ctx = translator.take_context();
                  auto result = llvm_aot_compile(std::move(llvm_ctx), std::move(llvm_mod),
                                                  get_ir_module(), target_triple, topts.opt_level);

                  if (!result.error.empty()) {
                     ws.had_error = true;
                     ws.error = std::move(result.error);
                     any_error.store(true, std::memory_order_relaxed);
                     return;
                  }

                  batch_results[batch_idx].code = std::move(result.code);
                  batch_results[batch_idx].relocations = std::move(result.relocations);
                  batch_results[batch_idx].function_offsets = std::move(result.function_offsets);
                  batch_results[batch_idx].body_offsets = std::move(result.body_offsets);
                  batch_results[batch_idx].entry_stack_sizes = std::move(result.entry_stack_sizes);
                  batch_results[batch_idx].body_stack_sizes = std::move(result.body_stack_sizes);
                  batch_results[batch_idx].import_thunk_offsets = std::move(result.import_thunk_offsets);
                  batch_results[batch_idx].import_thunk_stack_sizes = std::move(result.import_thunk_stack_sizes);
                  batch_results[batch_idx].native_guest_abi = result.native_guest_abi;
                  batch_results[batch_idx].native_stack_probe_size = result.native_stack_probe_size;
                  batch_results[batch_idx].native_stack_frames_complete = result.native_stack_frames_complete;
               };

#ifdef __EXCEPTIONS
               try {
                  do_compile();
               } catch (const std::exception& ex) {
                  ws.had_error = true;
                  ws.error = ex.what();
                  any_error.store(true, std::memory_order_relaxed);
               } catch (...) {
                  ws.had_error = true;
                  ws.error = "unknown error during parallel LLVM AOT compilation";
                  any_error.store(true, std::memory_order_relaxed);
               }
#else
               do_compile();
#endif
            },

            // between_fn: compute per-batch blob offsets, set global function offsets
            [this, &workers, &batch_results, num_batches, batch_size, N]() {
               // Check for errors
               for (uint32_t w = 0; w < N; ++w) {
                  if (workers[w].had_error) {
                     _compile_result->error = std::move(workers[w].error);
                     _had_error = true;
                     return;
                  }
               }

               // Compute per-batch blob offsets (prefix sum, 16-byte aligned)
               size_t total_size = 0;
               for (uint32_t b = 0; b < num_batches; ++b) {
                  size_t align_pad = (16 - (total_size % 16)) % 16;
                  total_size += align_pad;
                  batch_results[b].blob_offset = static_cast<uint32_t>(total_size);
                  total_size += batch_results[b].code.size();
                  if (total_size > uint64_t(INT32_MAX)) {
                     _compile_result->error = "LLVM merged native code exceeds relocation range";
                     _had_error = true;
                     return;
                  }
               }

               // Pre-allocate merged code blob
               _accumulated_code.resize(total_size, 0x00);

               // Set global function/body offsets from each batch's offsets
               for (uint32_t b = 0; b < num_batches; ++b) {
                  uint32_t blob_offset = batch_results[b].blob_offset;
                  uint32_t batch_start = b * batch_size;
                  uint32_t batch_end = std::min(batch_start + batch_size, _num_functions);
                  record_import_thunks(batch_results[b], blob_offset, b == 0);
                  if (_had_error) return;
                  for (uint32_t f = batch_start; f < batch_end; ++f) {
                     if (f < batch_results[b].function_offsets.size()) {
                        auto [entry_off, entry_sz] = batch_results[b].function_offsets[f];
                        if (entry_sz != 0) {
                           _function_offsets[f] = {blob_offset + entry_off, entry_sz};
                           record_native_frames(batch_results[b], f);
                        }
                     }
                     if (f < batch_results[b].body_offsets.size()) {
                        auto [body_off, body_sz] = batch_results[b].body_offsets[f];
                        if (body_sz != 0) {
                           _func_body_offsets[f] = {blob_offset + body_off, body_sz};
                        }
                     }
                  }
               }

               // Merge per-worker max_stack
               for (uint32_t w = 0; w < N; ++w) {
                  _mod.maximum_stack = std::max(_mod.maximum_stack, workers[w].max_stack);
               }
            },

            // merge_fn: parallel copy + reloc adjustment per batch
            [this, &batch_results, num_batches](uint32_t worker_id) {
               if (_had_error) return;
               uint32_t batches_per_worker = (num_batches + _compile_threads - 1) / _compile_threads;
               uint32_t start = worker_id * batches_per_worker;
               uint32_t end = std::min(start + batches_per_worker, num_batches);

               for (uint32_t b = start; b < end; ++b) {
                  auto& br = batch_results[b];
                  if (br.code.empty()) continue;

                  std::memcpy(_accumulated_code.data() + br.blob_offset,
                              br.code.data(), br.code.size());

                  for (auto& reloc : br.relocations) {
                     reloc.code_offset += br.blob_offset;
                     if (reloc.symbol == reloc_symbol::code_blob_self && reloc.addend >= 0) {
                        reloc.addend += static_cast<int32_t>(br.blob_offset);
                     }
                  }
               }
            }
         ); // end run_batch

         if (_had_error) return;

         // Collect all relocations in batch-index order for determinism
         for (uint32_t b = 0; b < num_batches; ++b) {
            for (auto& reloc : batch_results[b].relocations) {
               _accumulated_relocs.push_back(std::move(reloc));
            }
         }

         // Resolve cross-function relocations (negative-addend → final blob offset)
         resolve_cross_function_relocs();
         if (_had_error) return;
         finish_native_frames();
         if (_had_error) return;

         _compile_result->relocs = std::move(_accumulated_relocs);
         _compile_result->code_blob = std::move(_accumulated_code);
         _compile_result->function_offsets = std::move(_function_offsets);
         _compile_result->body_offsets = _func_body_offsets;
      }
#endif // !__wasi__

      void compile_current_function(bool imports_only = false) {
         if (_compile_result->target_triple.empty()) {
            _compile_result->error = "ir_writer_llvm_aot: target_triple not set";
            _had_error = true;
            return;
         }

         // Initialize function offsets on first call
         if (_function_offsets.empty()) {
            _function_offsets.resize(_num_functions, {0, 0});
            _func_body_offsets.resize(_num_functions, {0, 0});
         }

         // Create per-function LLVM translator + module
         llvm_translate_options topts;
         topts.opt_level        = _compile_result->opt_level;
         topts.fp               = _compile_result->softfloat ? fp_mode::softfloat
                                                             : fp_mode::hw_deterministic;
         topts.profile          = this->_float_profile;
         topts.per_function     = true;
         topts.enable_backtrace = _compile_result->backtrace;
         topts.mem_mode = _mem_mode;
         topts.checked_kind = _checked_kind;
         topts.emit_import_thunks = imports_only || _func->func_index == 0;

         auto do_compile = [&]() {
            llvm_ir_translator translator(get_ir_module(), topts);
            if (!imports_only) translator.translate_function(*_func);
            translator.finalize();

            auto llvm_mod = translator.take_module();
            auto llvm_ctx = translator.take_context();
            auto func_result = llvm_aot_compile(std::move(llvm_ctx), std::move(llvm_mod),
                                                 get_ir_module(), _compile_result->target_triple, topts.opt_level);

            if (!func_result.error.empty()) {
               _compile_result->error = std::move(func_result.error);
               _had_error = true;
               return;
            }

            // Align to 16 bytes before appending this function's code
            size_t align_pad = (16 - (_accumulated_code.size() % 16)) % 16;
            if (align_pad > 0) {
               _accumulated_code.resize(_accumulated_code.size() + align_pad, 0x00);
            }
            uint32_t blob_offset = static_cast<uint32_t>(_accumulated_code.size());
            if (uint64_t(blob_offset) + func_result.code.size() > uint64_t(INT32_MAX)) {
               _compile_result->error = "LLVM merged native code exceeds relocation range";
               _had_error = true;
               return;
            }

            _accumulated_code.insert(_accumulated_code.end(),
                                      func_result.code.begin(), func_result.code.end());
            record_import_thunks(func_result, blob_offset, topts.emit_import_thunks);
            if (_had_error) return;

            // Adjust relocation offsets and addends relative to merged blob
            for (auto& reloc : func_result.relocations) {
               reloc.code_offset += blob_offset;
               // code_blob_self addends reference positions within the per-function
               // blob and must be adjusted for the merged blob position.
               // Negative addends encode pending function refs (don't adjust those).
               if (reloc.symbol == reloc_symbol::code_blob_self && reloc.addend >= 0) {
                  reloc.addend += static_cast<int32_t>(blob_offset);
               }
               _accumulated_relocs.push_back(reloc);
            }

            // Record this function's entry wrapper and body offsets in the merged blob.
            // llvm_aot_compile returns per-function offsets relative to .text;
            // add blob_offset to get the absolute position in the merged blob.
            if (imports_only) return;
            uint32_t code_idx = _func->func_index;
            if (code_idx < _function_offsets.size() &&
                code_idx < func_result.function_offsets.size()) {
               auto [entry_off, entry_sz] = func_result.function_offsets[code_idx];
               _function_offsets[code_idx] = {blob_offset + entry_off, entry_sz};
            }
            if (code_idx < _func_body_offsets.size() &&
                code_idx < func_result.body_offsets.size()) {
               auto [body_off, body_sz] = func_result.body_offsets[code_idx];
               _func_body_offsets[code_idx] = {blob_offset + body_off, body_sz};
            }
            record_native_frames(func_result, code_idx);
         };

#ifdef __EXCEPTIONS
         try {
            do_compile();
         } catch (const std::exception& ex) {
            _compile_result->error = ex.what();
            _had_error = true;
         } catch (...) {
            _compile_result->error = "unknown error during LLVM AOT compilation";
            _had_error = true;
         }
#else
         do_compile();
#endif
      }

      std::vector<uint8_t> _accumulated_code;
      std::vector<code_relocation> _accumulated_relocs;
      std::vector<std::pair<uint32_t, uint32_t>> _function_offsets;  // wasm_entry_N offsets
      std::vector<std::pair<uint32_t, uint32_t>> _func_body_offsets; // wasm_func_N offsets
      std::vector<uint64_t> _entry_stack_sizes;
      std::vector<uint64_t> _body_stack_sizes;
      std::vector<uint8_t> _native_frame_presence;
      std::vector<std::pair<uint32_t, uint32_t>> _import_thunk_offsets;
      std::vector<uint64_t> _import_thunk_stack_sizes;
      bool _import_unit_present = false;
      bool _had_error = false;
   };

} // namespace psizam::detail
