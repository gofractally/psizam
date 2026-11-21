# Table prefix metadata

Table 0 may use the memory prefix for fixed-size entries. The inline region ends
at `wasm_allocator::mem_size_offset()`, before the stored 64-bit memory size,
32-bit table size and globals pointer. Its extent is derived from those offsets;
on 64-bit hosts the 20-byte metadata tail leaves at least 24 bytes after complete
16-byte table entries. Larger or growable tables use the existing indirect path.

Previously the capacity reserved only the globals pointer. A fixed table with
255 entries on a 4 KiB host, 1023 on a 16 KiB host, or 4095 on a 64 KiB host
overlapped the memory-size slot. `memory.grow` then corrupted the final code
pointer. The new regression grows memory before calling that final entry.

This corrects runtime layout selection without changing WASM bytes or the prefix
metadata offsets. Saved JIT native code embeds the old inline/indirect decision,
so matching the system page size is insufficient to establish compatibility.
PZAM version 5 invalidates version 4 artifacts; rebuild them from the original
WASM. Serialized and already-decoded loader entry points reject the old version.
The compiler identity includes the PZAM version and changes with this bump.
