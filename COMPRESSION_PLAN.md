# WDL Tablebase Compression Plan

## Overview

Compress WDL endgame tablebases using:
1. **"Don't care" positions**: Positions with captures available (or would be if opponent moved) are marked as placeholders - lookup performs a search to find the real value
2. **Block-based compression**: 16384 positions per block, each compressed independently using best-performing method
3. **In-memory compression**: Tablebases remain compressed in memory; blocks decompressed on-demand during lookup
4. **File format = memory format**: Near-direct mapping for fast load/save

## Key Design Decisions

- **Don't care encoding**: Placeholder value (0/1/2) chosen per block to optimize compression
- **Lookup search**: Unbounded depth with repetition detection (treat repetitions as draws)
- **Block access**: Random-access methods allow O(1)/O(log n) lookup; sequential methods use LRU cache
- **Values**: WIN=1, LOSS=2, DRAW=3 (current), but compressed using 0/1/2 ternary

---

## Stage 1: Don't-Care Detection and Marking

**Goal**: Create infrastructure to identify "don't care" positions.

### Tasks
1. Add function `is_dont_care(const Board& b)` that returns true if:
   - `has_captures(b)` returns true (position has captures available), OR
   - After flipping the board, `has_captures(flipped)` returns true

2. Create `generate_compressed_wdl()` that produces a vector where don't-care positions are marked with a special sentinel (e.g., 255 or -1 in a temporary buffer)

3. Add statistics gathering: count how many positions per material are don't-care vs real values

### Tests
- [ ] `is_dont_care()` correctly identifies capture positions
- [ ] `is_dont_care()` correctly identifies "opponent would capture" positions
- [ ] For material 000011 (2 pawns), verify don't-care count matches manual analysis
- [ ] Ensure don't-care positions still have correct underlying WDL values (for verification)

### Deliverable
- Function to mark don't-care positions
- Statistics showing compression potential (% of don't-care positions per material)

---

## Stage 2: Lookup with Search for Don't-Care Positions

**Goal**: Implement lookup that searches when hitting a don't-care position.

### Tasks
1. Create `lookup_wdl_with_search(const Board& b, visited_set)`:
   - If position is not don't-care, return stored value
   - Otherwise, generate moves, recursively evaluate successors
   - Track visited positions (by hash) for repetition detection
   - Repetition = DRAW

2. Implement minimax logic:
   - If any successor is LOSS for opponent → we WIN
   - If all successors are WIN for opponent → we LOSS
   - Otherwise → DRAW

3. Optimize for speed:
   - Use board hash for visited set (std::unordered_set)
   - Consider alpha-beta or other pruning if needed
   - Profile typical search depth

### Tests
- [ ] Lookup returns correct WDL for non-don't-care positions (direct)
- [ ] Lookup returns correct WDL for don't-care positions (via search)
- [ ] Repetition detection works: cyclic position → DRAW
- [ ] Verify against original uncompressed tablebase for random positions
- [ ] Performance: measure average search depth and lookup time

### Deliverable
- Working lookup function that handles don't-care via search
- Benchmark data on search overhead

---

## Stage 3: Block-Based Compression Infrastructure

**Goal**: Create the block structure and compression framework.

### Data Structures
```cpp
struct CompressedBlock {
  uint8_t method;           // Compression method ID
  uint16_t compressed_size; // Size of compressed data
  uint8_t data[];           // Variable-length compressed data
};

struct CompressedTablebase {
  Material material;
  uint32_t num_blocks;
  uint32_t block_offsets[];  // Offset to each block's data
  uint8_t block_data[];      // Concatenated compressed blocks
};

// LRU cache for decompressed blocks
struct BlockCache {
  static constexpr size_t CACHE_SIZE = 16;  // Configurable
  std::list<std::pair<uint32_t, std::vector<Value>>> lru_list;
  std::unordered_map<uint32_t, decltype(lru_list)::iterator> cache_map;

  Value* get_or_decompress(uint32_t block_idx, CompressedTablebase& tb);
};
```

### Tasks
1. Define block size constant: `BLOCK_SIZE = 16384`
2. Create `CompressedTablebase` structure for in-memory storage
3. Implement `decompress_block(block_data, method) → vector<Value>`
4. Implement `compress_block(values, method) → vector<uint8_t>`
5. Create framework to try multiple methods and pick smallest

### Compression Methods (Initial Set)

**Random-access methods** (no decompression needed):
- Method 0: Raw (2 bits per value, 4 per byte) - baseline
- Method 1: Ternary base-3 (5 values per byte) - for 3-value blocks

**Sequential-access methods** (require full block decompression, use LRU cache):
- (Added in Stage 4)

### Tests
- [ ] Round-trip: compress then decompress equals original
- [ ] Block indexing: correct block selected for any position index
- [ ] Method 0 produces expected size (4096 bytes for 16384 positions)
- [ ] Method 1 produces expected size (~3277 bytes for 16384 ternary values)
- [ ] LRU cache eviction works correctly (oldest evicted when full)
- [ ] Cache hit rate is reasonable for sequential access patterns

### Deliverable
- Block compression/decompression framework
- Two working compression methods

---

## Stage 4: Advanced Compression Methods

**Goal**: Implement remaining compression methods.

### Methods to Implement

**Random-access methods** (O(1) or O(log n) lookup within block):

2. **Method 2: Default + Exceptions (sorted)**
   - Store: default_value, exception_count, sorted list of (index, value) pairs
   - Lookup: binary search in exception list, return default if not found
   - Good for blocks with one dominant value

3. **Method 3: RLE with Binary Search**
   - Store: cumulative run lengths + values
   - Lookup: binary search on cumulative lengths to find run
   - Good for blocks with long runs

**Sequential-access methods** (require full decompression, use LRU cache):

4. **Method 4-7: Huffman RLE variants**
   - Different frequency tables for run lengths
   - Encode (value, run_length) pairs with Huffman codes
   - Must decompress entire block to access any position
   - Good for complex patterns with variable run lengths

### Tasks
1. Implement each compression method's encode/decode
2. Create `select_best_method(block_data) → (method_id, compressed_data)`
3. Profile compression ratio across different materials

### Tests
- [ ] Each method round-trips correctly
- [ ] Method selection consistently picks smallest output
- [ ] Compression ratio improvement over baseline (target: 2-3x over raw 2-bit)
- [ ] Decompression speed acceptable (benchmark full block decode)

### Deliverable
- Complete set of compression methods
- Automatic method selection per block
- Compression statistics per material

---

## Stage 5: File Format and I/O

**Goal**: Save/load compressed tablebases efficiently.

### File Format
```
Header:
  magic: "CWDL" (4 bytes)
  version: uint8
  material: Material struct (24 bytes)
  num_blocks: uint32
  total_size: uint64 (for mmap compatibility)

Block Index:
  offsets[num_blocks]: uint32[] (offset from data start)

Block Data:
  For each block:
    method: uint8
    compressed_size: uint16
    data: uint8[compressed_size]
```

### Tasks
1. Implement `save_compressed_tablebase()`
2. Implement `load_compressed_tablebase()`
3. Consider mmap for large tablebases (optional optimization)
4. Add version check for forward compatibility

### Tests
- [ ] Save then load produces identical in-memory structure
- [ ] File size matches expected compressed size
- [ ] Load time is fast (benchmark vs current uncompressed load)
- [ ] Corrupt file detection (magic/version check)

### Deliverable
- Compressed file I/O
- File format documentation

---

## Stage 6: Integration and Migration

**Goal**: Integrate compressed tablebases into the main codebase.

### Tasks
1. Create new lookup API: `lookup_compressed_wdl(CompressedTablebase&, Board&)`
2. Add `--compress` flag to generate tool for creating compressed TBs
3. Add `--use-compressed` flag for using compressed TBs in lookup/verify
4. Create migration script to compress existing tablebases
5. Update verify tool to work with compressed format

### Tests
- [ ] Generate compressed 4-piece tablebases
- [ ] Verify compressed TBs match original TBs for all positions
- [ ] Benchmark: lookup speed comparison (compressed vs uncompressed)
- [ ] Memory usage comparison
- [ ] Full verification pass with compressed TBs

### Deliverable
- Complete compressed tablebase pipeline
- Migration path from uncompressed to compressed
- Performance comparison report

---

## Files to Modify/Create

### New Files
- `compression.h` - Compression methods, block structures
- `compression.cpp` - Compression implementation
- `compressed_tablebase.h` - CompressedTablebase structure
- `compressed_tablebase.cpp` - Load/save, lookup with search

### Modified Files
- `tablebase.h` - Add compressed format declarations
- `generate.cpp` - Add --compress option
- `lookup.cpp` - Support compressed lookup
- `verify.cpp` - Support compressed verification

---

## Success Metrics

1. **Compression ratio**: Target 3-5x smaller than current 2-bit format
2. **Lookup speed**: < 2x slowdown for random access (including search overhead)
3. **Memory reduction**: Proportional to compression ratio
4. **Load time**: Faster than uncompressed (less I/O)

---

## Design Decisions (Resolved)

1. **Block caching**: LRU cache for sequential-access methods only (Huffman RLE); random-access methods (base-3, default+exceptions, RLE with binary search) don't need caching
2. **Search memoization**: No, re-search don't-care positions each time (searches are fast)
3. **Memory format = file format**: Direct mapping for fast load/save

## Future Considerations

1. Lazy block loading for very large tablebases?
2. DTM compression with similar approach?
3. Optimal LRU cache size (start with 16 blocks, tune based on profiling)?
