# Native integration checkpoint

The psibase integration uses the core runtime interface with the existing serialized VM limits. Browser plugins continue to use the browser WebAssembly implementation.

Three standalone changes support that integration:

- A scoped thread-local observer records explicit traps for diagnostics without introducing callbacks into asynchronous signal handlers.
- The x86 unreachable instruction preserves its active frame for debugger backtraces.
- An opt-in parser table-growth policy applies before code generation, so limiting a table cannot change the storage layout after indirect calls have already been compiled. The default standalone policy is unchanged.

The core changes at `ff68c56fc5e60ccc969d2a1101d251baf92c67e5` were rebuilt and tested on ARM64 macOS and the dedicated x86-64 Linux server, using the header-only psio fix `f6b10ec7d7bba4022cd1c9394f88ad1440b4b2b7`. Both platforms passed the complete standalone unit, parity, compliance, diagnostics, table, quick-execution, source-consumer and installed-consumer gates. The psibase adapter additionally passed the native transaction and bounded alternate-context timeout/recovery gates on both platforms. Compiler-workload qualification remains a separate opt-in workflow.

The workflow pin now uses that same psio header fix. It adds direct standard-library includes to the annotation header; it does not select the in-progress PSSZ wire-format changes. The earlier standalone publication tag remains immutable. A later checkpoint tag requires the hosted Linux/macOS standalone workflow to pass on the review branch.
