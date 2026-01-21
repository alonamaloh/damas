#include "compression.h"
#include "movegen.h"
#include <algorithm>

// ============================================================================
// Don't-Care Position Detection (Stage 1)
// ============================================================================

bool is_dont_care(const Board& b) {
  // Only mark positions where WE have captures as don't-care.
  // These are truly forced (mandatory capture rule) and lead to material change.
  //
  // Note: The original plan also included "opponent would have captures" positions,
  // but these can lead to very long search chains through quiet moves, causing
  // performance issues. A future optimization could handle these differently
  // (e.g., limited depth search or separate treatment).
  return has_captures(b);
}

// ============================================================================
// Compression Statistics Analysis
// ============================================================================

CompressionStats analyze_compression(const std::vector<Value>& tablebase, const Material& m) {
  CompressionStats stats;
  stats.total_positions = tablebase.size();

  for (std::size_t idx = 0; idx < tablebase.size(); ++idx) {
    Board board = index_to_board(idx, m);
    Value val = tablebase[idx];

    // Check if this is a don't-care position (only "we have captures")
    bool we_have_captures = has_captures(board);

    if (we_have_captures) {
      stats.dont_care_positions++;
    } else {
      stats.real_positions++;
    }

    // Also track opponent captures for analysis (not used for don't-care)
    if (!we_have_captures && has_captures(flip(board))) {
      stats.opponent_capture_positions++;
    }

    // Count values
    switch (val) {
      case Value::WIN: stats.wins++; break;
      case Value::LOSS: stats.losses++; break;
      case Value::DRAW: stats.draws++; break;
      default: break;
    }
  }

  return stats;
}

// ============================================================================
// Mark Don't-Care Positions
// ============================================================================

std::vector<Value> mark_dont_care_positions(
    const std::vector<Value>& original,
    const Material& m,
    CompressionStats& stats) {

  stats = CompressionStats{};
  stats.total_positions = original.size();

  std::vector<Value> result(original.size());

  for (std::size_t idx = 0; idx < original.size(); ++idx) {
    Board board = index_to_board(idx, m);
    Value val = original[idx];

    // Check if this is a don't-care position (only "we have captures")
    bool we_have_captures = has_captures(board);

    if (we_have_captures) {
      stats.dont_care_positions++;
      result[idx] = Value::UNKNOWN;  // Use UNKNOWN as sentinel for don't-care
    } else {
      stats.real_positions++;
      result[idx] = val;
    }

    // Also track opponent captures for analysis (not used for don't-care)
    if (!we_have_captures && has_captures(flip(board))) {
      stats.opponent_capture_positions++;
    }

    // Count original values
    switch (val) {
      case Value::WIN: stats.wins++; break;
      case Value::LOSS: stats.losses++; break;
      case Value::DRAW: stats.draws++; break;
      default: break;
    }
  }

  return result;
}

// ============================================================================
// WDL Lookup with Search (Stage 2)
// ============================================================================

namespace {

// Maximum search depth to prevent stack overflow
// This is conservative - in practice, don't-care chains should be short
constexpr std::size_t MAX_SEARCH_DEPTH = 500;

// Internal recursive search with cycle detection and statistics
struct SearchContext {
  const std::unordered_map<Material, std::vector<Value>>* sub_tablebases;
  SearchStats* stats;
  std::size_t current_depth;
  std::size_t nodes_this_search;
};

// Internal recursive search with cycle detection
// Returns the WDL value from the perspective of the side to move
Value search_wdl_internal(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m,
    std::unordered_set<std::uint64_t>& visited,
    SearchContext& ctx) {

  ctx.nodes_this_search++;
  ctx.current_depth++;

  if (ctx.stats && ctx.current_depth > ctx.stats->max_depth) {
    ctx.stats->max_depth = ctx.current_depth;
  }

  // Depth limit to prevent stack overflow
  // If we exceed the limit, conservatively return DRAW
  if (ctx.current_depth > MAX_SEARCH_DEPTH) {
    ctx.current_depth--;
    return Value::DRAW;
  }

  // Check for cycle (repetition = draw)
  std::uint64_t hash = b.hash();
  if (visited.count(hash)) {
    if (ctx.stats) ctx.stats->cycles_detected++;
    ctx.current_depth--;
    return Value::DRAW;
  }

  // Get the stored value
  std::size_t idx = board_to_index(b, m);
  Value stored = tablebase[idx];

  // If not don't-care, return the stored value
  if (stored != Value::UNKNOWN) {
    ctx.current_depth--;
    return stored;
  }

  // Mark as visited for cycle detection
  visited.insert(hash);

  // Generate moves and search
  std::vector<Move> moves;
  generateMoves(b, moves);

  // No moves = loss (can't happen from don't-care, but handle defensively)
  if (moves.empty()) {
    visited.erase(hash);
    ctx.current_depth--;
    return Value::LOSS;
  }

  // Minimax: if any successor is LOSS for opponent, we WIN
  //          if all successors are WIN for opponent, we LOSS
  //          otherwise, DRAW
  bool has_draw = false;

  for (const Move& move : moves) {
    Board next = makeMove(b, move);

    Value successor_value;

    // Terminal check: if opponent has no pieces, they lose (we win)
    // After makeMove, next.white is the opponent's pieces from their perspective
    if (next.white == 0) {
      // Opponent has no pieces = terminal loss for them = win for us
      if (ctx.stats) ctx.stats->terminal_wins++;
      successor_value = Value::LOSS;
    } else {
      Material next_m = get_material(next);

      // Check if material changed (capture occurred)
      if (!(next_m == flip(m))) {
        // Material changed - look up in sub-tablebase
        if (ctx.sub_tablebases) {
          auto it = ctx.sub_tablebases->find(next_m);
          if (it != ctx.sub_tablebases->end() && !it->second.empty()) {
            if (ctx.stats) ctx.stats->sub_tb_lookups++;
            std::size_t sub_idx = board_to_index(next, next_m);
            if (sub_idx < it->second.size()) {
              Value sub_val = it->second[sub_idx];
              // If sub-tablebase also has don't-care, recurse into it
              if (sub_val == Value::UNKNOWN) {
                successor_value = search_wdl_internal(next, it->second, next_m, visited, ctx);
              } else {
                successor_value = sub_val;
              }
            } else {
              // Index out of range - shouldn't happen
              successor_value = Value::DRAW;
            }
          } else {
            // Sub-tablebase not available - conservative: treat as draw
            successor_value = Value::DRAW;
          }
        } else {
          // No sub-tablebases provided - conservative: treat as draw
          successor_value = Value::DRAW;
        }
      } else {
        // Same material class - recurse
        successor_value = search_wdl_internal(next, tablebase, flip(m), visited, ctx);
      }
    }

    // Opponent's loss is our win
    if (successor_value == Value::LOSS) {
      visited.erase(hash);
      ctx.current_depth--;
      return Value::WIN;
    }

    if (successor_value == Value::DRAW) {
      has_draw = true;
    }
  }

  visited.erase(hash);
  ctx.current_depth--;

  // If we found a draw, we can draw; otherwise all moves lead to opponent winning
  return has_draw ? Value::DRAW : Value::LOSS;
}

// Simple version without sub-tablebases (for backward compatibility)
Value search_wdl_simple(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m,
    std::unordered_set<std::uint64_t>& visited) {

  SearchContext ctx{nullptr, nullptr, 0, 0};
  return search_wdl_internal(b, tablebase, m, visited, ctx);
}

} // anonymous namespace

Value lookup_wdl_with_search(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m) {

  std::unordered_set<std::uint64_t> visited;
  return search_wdl_simple(b, tablebase, m, visited);
}

Value lookup_wdl_with_search(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& sub_tablebases,
    SearchStats* stats) {

  if (stats) stats->lookups++;

  // Check if it's a direct hit (not don't-care)
  std::size_t idx = board_to_index(b, m);
  Value stored = tablebase[idx];
  if (stored != Value::UNKNOWN) {
    if (stats) stats->direct_hits++;
    return stored;
  }

  // Need to search
  if (stats) stats->searches++;

  std::unordered_set<std::uint64_t> visited;
  SearchContext ctx{&sub_tablebases, stats, 0, 0};

  Value result = search_wdl_internal(b, tablebase, m, visited, ctx);

  if (stats) stats->total_nodes += ctx.nodes_this_search;

  return result;
}

// ============================================================================
// Block Compression/Decompression (Stage 3)
// ============================================================================

namespace {

// Value encoding for compression:
// Map Value enum to small integers for compact storage.
// We use: WIN=0, DRAW=1, LOSS=2, UNKNOWN=3
inline std::uint8_t value_to_int(Value v) {
  switch (v) {
    case Value::WIN: return 0;
    case Value::DRAW: return 1;
    case Value::LOSS: return 2;
    case Value::UNKNOWN: return 3;
    default: return 3;  // Treat invalid as UNKNOWN
  }
}

inline Value int_to_value(std::uint8_t i) {
  switch (i) {
    case 0: return Value::WIN;
    case 1: return Value::DRAW;
    case 2: return Value::LOSS;
    case 3: return Value::UNKNOWN;
    default: return Value::UNKNOWN;
  }
}

// ============================================================================
// Method 0: RAW_2BIT
// 2 bits per value, 4 values per byte
// ============================================================================

std::vector<std::uint8_t> compress_raw_2bit(const Value* values, std::size_t count) {
  std::size_t num_bytes = (count + 3) / 4;
  std::vector<std::uint8_t> result(num_bytes, 0);

  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    std::size_t byte_idx = i / 4;
    std::size_t bit_pos = (i % 4) * 2;
    result[byte_idx] |= (v << bit_pos);
  }

  return result;
}

std::vector<Value> decompress_raw_2bit(const std::uint8_t* data, std::size_t num_values) {
  std::vector<Value> result(num_values);

  for (std::size_t i = 0; i < num_values; ++i) {
    std::size_t byte_idx = i / 4;
    std::size_t bit_pos = (i % 4) * 2;
    std::uint8_t v = (data[byte_idx] >> bit_pos) & 0x3;
    result[i] = int_to_value(v);
  }

  return result;
}

// ============================================================================
// Method 1: TERNARY_BASE3
// Base-3 encoding, 5 values per byte (3^5 = 243 < 256)
// Only works for blocks with at most 3 distinct values.
// ============================================================================

// Check if a block can be ternary-encoded (has <= 3 distinct values)
bool can_use_ternary(const Value* values, std::size_t count) {
  bool seen[4] = {false, false, false, false};
  int num_distinct = 0;

  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (!seen[v]) {
      seen[v] = true;
      ++num_distinct;
      if (num_distinct > 3) return false;
    }
  }

  return true;
}

// Compress using base-3 encoding.
// First byte stores the mapping: which 3 values are present (encoded as bitmask).
// Remaining bytes store 5 ternary values each.
std::vector<std::uint8_t> compress_ternary_base3(const Value* values, std::size_t count) {
  // First, determine which values are present and create mapping
  bool present[4] = {false, false, false, false};
  for (std::size_t i = 0; i < count; ++i) {
    present[value_to_int(values[i])] = true;
  }

  // Create mapping: value_to_int(v) -> ternary index (0, 1, or 2)
  std::uint8_t mapping[4];
  std::uint8_t reverse_mapping[3];  // ternary index -> original value
  std::uint8_t next_idx = 0;

  for (int v = 0; v < 4 && next_idx < 3; ++v) {
    if (present[v]) {
      mapping[v] = next_idx;
      reverse_mapping[next_idx] = static_cast<std::uint8_t>(v);
      ++next_idx;
    } else {
      mapping[v] = 0;  // Map unused values to 0
    }
  }

  // Fill remaining slots in reverse mapping (shouldn't matter, but be safe)
  while (next_idx < 3) {
    reverse_mapping[next_idx++] = 0;
  }

  // Calculate output size: 1 header byte + ceil(count/5) data bytes
  std::size_t num_data_bytes = (count + 4) / 5;
  std::vector<std::uint8_t> result;
  result.reserve(1 + num_data_bytes);

  // Header byte: encodes the reverse mapping
  // Bits 0-1: reverse_mapping[0]
  // Bits 2-3: reverse_mapping[1]
  // Bits 4-5: reverse_mapping[2]
  std::uint8_t header = reverse_mapping[0] |
                        (reverse_mapping[1] << 2) |
                        (reverse_mapping[2] << 4);
  result.push_back(header);

  // Encode 5 values per byte in base-3
  for (std::size_t i = 0; i < count; i += 5) {
    std::uint8_t packed = 0;
    std::uint8_t multiplier = 1;

    for (std::size_t j = 0; j < 5 && i + j < count; ++j) {
      std::uint8_t ternary_val = mapping[value_to_int(values[i + j])];
      packed += ternary_val * multiplier;
      multiplier *= 3;
    }

    result.push_back(packed);
  }

  return result;
}

std::vector<Value> decompress_ternary_base3(const std::uint8_t* data, std::size_t num_values) {
  if (num_values == 0) return {};

  // Read header to get reverse mapping
  std::uint8_t header = data[0];
  std::uint8_t reverse_mapping[3];
  reverse_mapping[0] = header & 0x3;
  reverse_mapping[1] = (header >> 2) & 0x3;
  reverse_mapping[2] = (header >> 4) & 0x3;

  std::vector<Value> result(num_values);
  const std::uint8_t* packed = data + 1;

  for (std::size_t i = 0; i < num_values; i += 5) {
    std::uint8_t byte = packed[i / 5];

    for (std::size_t j = 0; j < 5 && i + j < num_values; ++j) {
      std::uint8_t ternary_val = byte % 3;
      byte /= 3;
      result[i + j] = int_to_value(reverse_mapping[ternary_val]);
    }
  }

  return result;
}

} // anonymous namespace

std::vector<std::uint8_t> compress_block(
    const Value* values,
    std::size_t count,
    CompressionMethod method) {

  switch (method) {
    case CompressionMethod::RAW_2BIT:
      return compress_raw_2bit(values, count);

    case CompressionMethod::TERNARY_BASE3:
      if (can_use_ternary(values, count)) {
        return compress_ternary_base3(values, count);
      }
      // Fall back to raw 2-bit if ternary not possible
      return compress_raw_2bit(values, count);

    case CompressionMethod::DEFAULT_EXCEPTIONS:
    case CompressionMethod::RLE_BINARY_SEARCH:
      // Not implemented yet - fall back to raw 2-bit
      return compress_raw_2bit(values, count);

    default:
      return compress_raw_2bit(values, count);
  }
}

std::vector<Value> decompress_block(
    const std::uint8_t* data,
    std::size_t /*data_size*/,
    std::size_t num_values,
    CompressionMethod method) {

  switch (method) {
    case CompressionMethod::RAW_2BIT:
      return decompress_raw_2bit(data, num_values);

    case CompressionMethod::TERNARY_BASE3:
      return decompress_ternary_base3(data, num_values);

    case CompressionMethod::DEFAULT_EXCEPTIONS:
    case CompressionMethod::RLE_BINARY_SEARCH:
      // Not implemented yet - assume raw 2-bit
      return decompress_raw_2bit(data, num_values);

    default:
      return decompress_raw_2bit(data, num_values);
  }
}

std::pair<CompressionMethod, std::vector<std::uint8_t>> compress_block_best(
    const Value* values,
    std::size_t count) {

  // Try Method 0: RAW_2BIT (always available)
  auto raw = compress_raw_2bit(values, count);
  CompressionMethod best_method = CompressionMethod::RAW_2BIT;
  std::vector<std::uint8_t> best_data = std::move(raw);

  // Try Method 1: TERNARY_BASE3 (if applicable)
  if (can_use_ternary(values, count)) {
    auto ternary = compress_ternary_base3(values, count);
    if (ternary.size() < best_data.size()) {
      best_method = CompressionMethod::TERNARY_BASE3;
      best_data = std::move(ternary);
    }
  }

  // Methods 2 and 3 not implemented yet

  return {best_method, std::move(best_data)};
}

std::size_t expected_compressed_size(std::size_t num_values, CompressionMethod method) {
  switch (method) {
    case CompressionMethod::RAW_2BIT:
      return (num_values + 3) / 4;

    case CompressionMethod::TERNARY_BASE3:
      return 1 + (num_values + 4) / 5;  // 1 header byte + data

    default:
      return 0;  // Cannot determine without compressing
  }
}

// ============================================================================
// LRU Cache Implementation (Stage 3)
// ============================================================================

BlockCache::BlockCache(std::size_t max_size)
    : max_size_(max_size > 0 ? max_size : DEFAULT_CACHE_SIZE) {}

const std::vector<Value>* BlockCache::get_or_decompress(
    std::uint32_t block_idx,
    const CompressedTablebase& tb) {

  // Check if already in cache
  auto it = cache_map_.find(block_idx);
  if (it != cache_map_.end()) {
    // Cache hit - move to front
    hits_++;
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    return &it->second->second;
  }

  // Cache miss - decompress the block
  misses_++;

  // Get block info
  if (block_idx >= tb.num_blocks) {
    return nullptr;
  }

  std::uint32_t block_offset = tb.block_offsets[block_idx];
  const std::uint8_t* block_ptr = tb.block_data.data() + block_offset;

  // Read block header
  CompressionMethod method = static_cast<CompressionMethod>(block_ptr[0]);
  std::uint16_t compressed_size = block_ptr[1] | (block_ptr[2] << 8);
  const std::uint8_t* data = block_ptr + 3;

  // Calculate number of values in this block
  std::size_t block_start = static_cast<std::size_t>(block_idx) * BLOCK_SIZE;
  std::size_t block_end = std::min(block_start + BLOCK_SIZE,
                                   static_cast<std::size_t>(tb.num_positions));
  std::size_t num_values = block_end - block_start;

  // Decompress
  std::vector<Value> decompressed = decompress_block(data, compressed_size, num_values, method);

  // Evict oldest if cache is full
  if (lru_list_.size() >= max_size_) {
    auto& oldest = lru_list_.back();
    cache_map_.erase(oldest.first);
    lru_list_.pop_back();
  }

  // Insert at front
  lru_list_.emplace_front(block_idx, std::move(decompressed));
  cache_map_[block_idx] = lru_list_.begin();

  return &lru_list_.front().second;
}

void BlockCache::clear() {
  lru_list_.clear();
  cache_map_.clear();
  hits_ = 0;
  misses_ = 0;
}

// ============================================================================
// CompressedTablebase Creation and Lookup (Stage 3)
// ============================================================================

CompressedTablebase compress_tablebase(
    const std::vector<Value>& values,
    const Material& m) {

  CompressedTablebase tb;
  tb.material = m;
  tb.num_positions = static_cast<std::uint32_t>(values.size());
  tb.num_blocks = (tb.num_positions + BLOCK_SIZE - 1) / BLOCK_SIZE;

  tb.block_offsets.reserve(tb.num_blocks);

  // Compress each block
  for (std::uint32_t block_idx = 0; block_idx < tb.num_blocks; ++block_idx) {
    std::size_t block_start = static_cast<std::size_t>(block_idx) * BLOCK_SIZE;
    std::size_t block_end = std::min(block_start + BLOCK_SIZE, values.size());
    std::size_t num_values = block_end - block_start;

    // Record offset
    tb.block_offsets.push_back(static_cast<std::uint32_t>(tb.block_data.size()));

    // Find best compression
    auto [method, compressed] = compress_block_best(values.data() + block_start, num_values);

    // Write block header: method (1 byte) + size (2 bytes) + data
    tb.block_data.push_back(static_cast<std::uint8_t>(method));
    std::uint16_t size = static_cast<std::uint16_t>(compressed.size());
    tb.block_data.push_back(size & 0xFF);
    tb.block_data.push_back((size >> 8) & 0xFF);

    // Write compressed data
    tb.block_data.insert(tb.block_data.end(), compressed.begin(), compressed.end());
  }

  return tb;
}

Value lookup_compressed(
    const CompressedTablebase& tb,
    std::size_t index,
    BlockCache* cache) {

  if (index >= tb.num_positions) {
    return Value::UNKNOWN;
  }

  std::uint32_t block_idx = static_cast<std::uint32_t>(index / BLOCK_SIZE);
  std::size_t idx_in_block = index % BLOCK_SIZE;

  // Get block info
  std::uint32_t block_offset = tb.block_offsets[block_idx];
  const std::uint8_t* block_ptr = tb.block_data.data() + block_offset;
  CompressionMethod method = static_cast<CompressionMethod>(block_ptr[0]);

  // For random-access methods, we can decode directly
  if (method == CompressionMethod::RAW_2BIT) {
    const std::uint8_t* data = block_ptr + 3;
    std::size_t byte_idx = idx_in_block / 4;
    std::size_t bit_pos = (idx_in_block % 4) * 2;
    std::uint8_t v = (data[byte_idx] >> bit_pos) & 0x3;
    return int_to_value(v);
  }

  if (method == CompressionMethod::TERNARY_BASE3) {
    const std::uint8_t* data = block_ptr + 3;
    std::uint8_t header = data[0];
    std::uint8_t reverse_mapping[3];
    reverse_mapping[0] = header & 0x3;
    reverse_mapping[1] = (header >> 2) & 0x3;
    reverse_mapping[2] = (header >> 4) & 0x3;

    // Find the byte containing this value
    std::size_t group_idx = idx_in_block / 5;
    std::size_t pos_in_group = idx_in_block % 5;
    std::uint8_t byte = data[1 + group_idx];

    // Extract the ternary value at the given position
    for (std::size_t i = 0; i < pos_in_group; ++i) {
      byte /= 3;
    }
    std::uint8_t ternary_val = byte % 3;

    return int_to_value(reverse_mapping[ternary_val]);
  }

  // For sequential-access methods, use cache
  if (cache) {
    const std::vector<Value>* block = cache->get_or_decompress(block_idx, tb);
    if (block && idx_in_block < block->size()) {
      return (*block)[idx_in_block];
    }
  }

  // Fallback: decompress entire block (inefficient, but correct)
  std::uint16_t compressed_size = block_ptr[1] | (block_ptr[2] << 8);
  const std::uint8_t* data = block_ptr + 3;

  std::size_t block_start = static_cast<std::size_t>(block_idx) * BLOCK_SIZE;
  std::size_t block_end = std::min(block_start + BLOCK_SIZE,
                                   static_cast<std::size_t>(tb.num_positions));
  std::size_t num_values = block_end - block_start;

  auto decompressed = decompress_block(data, compressed_size, num_values, method);
  if (idx_in_block < decompressed.size()) {
    return decompressed[idx_in_block];
  }

  return Value::UNKNOWN;
}

Value lookup_compressed_with_search(
    const Board& b,
    const CompressedTablebase& tb,
    BlockCache* cache) {

  std::size_t idx = board_to_index(b, tb.material);
  Value stored = lookup_compressed(tb, idx, cache);

  // If not don't-care, return directly
  if (stored != Value::UNKNOWN) {
    return stored;
  }

  // Need to search - for now, decompress entire tablebase and use existing search
  // This is a simple implementation; could be optimized later
  std::vector<Value> full_tb(tb.num_positions);
  for (std::size_t i = 0; i < tb.num_positions; ++i) {
    full_tb[i] = lookup_compressed(tb, i, cache);
  }

  return lookup_wdl_with_search(b, full_tb, tb.material);
}

// ============================================================================
// Compression Statistics (Stage 3)
// ============================================================================

BlockCompressionStats analyze_block_compression(const CompressedTablebase& tb) {
  BlockCompressionStats stats;
  stats.total_blocks = tb.num_blocks;
  stats.uncompressed_size = tb.num_positions;  // 1 byte per value uncompressed

  for (std::uint32_t block_idx = 0; block_idx < tb.num_blocks; ++block_idx) {
    std::uint32_t block_offset = tb.block_offsets[block_idx];
    const std::uint8_t* block_ptr = tb.block_data.data() + block_offset;

    CompressionMethod method = static_cast<CompressionMethod>(block_ptr[0]);
    std::uint16_t compressed_size = block_ptr[1] | (block_ptr[2] << 8);

    if (static_cast<int>(method) < 4) {
      stats.method_counts[static_cast<int>(method)]++;
    }

    // Block header (3 bytes) + compressed data
    stats.compressed_size += 3 + compressed_size;
  }

  return stats;
}
