#include "compression.h"
#include "movegen.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <cmath>

// Forward declarations for Huffman RLE functions
std::vector<std::uint8_t> compress_huffman_rle(const Value* values, std::size_t count,
                                               CompressionMethod method);
std::vector<Value> decompress_huffman_rle(const std::uint8_t* data, std::size_t data_size,
                                          std::size_t num_values, CompressionMethod method);

// Forward declarations for optimized 2-value RLE
std::vector<std::uint8_t> compress_rle_huffman_2val(const Value* values, std::size_t count);
std::vector<Value> decompress_rle_huffman_2val(const std::uint8_t* data, std::size_t data_size,
                                                std::size_t num_values);

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

// ============================================================================
// Method 3: RLE_BINARY_SEARCH
// Run-length encoding with 16-bit records: 14-bit index + 2-bit value.
// Each record means "from this index until the next record, value is X".
// Lookup is O(log n) via binary search on indices.
// ============================================================================

// Compress using RLE with 16-bit records.
// Format: sequence of 16-bit little-endian records
//   Bits 0-13: start index (0-16383)
//   Bits 14-15: value (0-3)
// Records are sorted by index. The last record covers to end of block.
std::vector<std::uint8_t> compress_rle_binary_search(const Value* values, std::size_t count) {
  if (count == 0) return {};

  std::vector<std::uint8_t> result;
  result.reserve(count / 100 * 2);  // Estimate: ~1% run changes

  std::uint8_t current_value = value_to_int(values[0]);

  // First record always starts at index 0
  std::uint16_t record = (static_cast<std::uint16_t>(current_value) << 14) | 0;
  result.push_back(record & 0xFF);
  result.push_back((record >> 8) & 0xFF);

  for (std::size_t i = 1; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (v != current_value) {
      // New run starts at index i
      current_value = v;
      record = (static_cast<std::uint16_t>(v) << 14) | static_cast<std::uint16_t>(i);
      result.push_back(record & 0xFF);
      result.push_back((record >> 8) & 0xFF);
    }
  }

  return result;
}

std::vector<Value> decompress_rle_binary_search(const std::uint8_t* data, std::size_t data_size, std::size_t num_values) {
  if (num_values == 0 || data_size < 2) return std::vector<Value>(num_values, Value::UNKNOWN);

  std::vector<Value> result(num_values);
  std::size_t num_records = data_size / 2;

  // Decode all records
  std::vector<std::pair<std::size_t, Value>> runs;
  runs.reserve(num_records);

  for (std::size_t i = 0; i < num_records; ++i) {
    std::uint16_t record = data[i * 2] | (data[i * 2 + 1] << 8);
    std::size_t start_idx = record & 0x3FFF;
    std::uint8_t val = (record >> 14) & 0x3;
    runs.emplace_back(start_idx, int_to_value(val));
  }

  // Fill result by iterating through runs
  for (std::size_t r = 0; r < runs.size(); ++r) {
    std::size_t start = runs[r].first;
    std::size_t end = (r + 1 < runs.size()) ? runs[r + 1].first : num_values;
    Value val = runs[r].second;

    for (std::size_t i = start; i < end; ++i) {
      result[i] = val;
    }
  }

  return result;
}

// Lookup a single value using binary search on RLE records.
// Returns the value at the given index.
Value lookup_rle_binary_search(const std::uint8_t* data, std::size_t data_size, std::size_t index) {
  std::size_t num_records = data_size / 2;
  if (num_records == 0) return Value::UNKNOWN;

  // Binary search to find the record that covers this index.
  // We want the largest start_idx <= index.
  std::size_t lo = 0, hi = num_records;

  while (lo < hi) {
    std::size_t mid = lo + (hi - lo) / 2;
    std::uint16_t record = data[mid * 2] | (data[mid * 2 + 1] << 8);
    std::size_t start_idx = record & 0x3FFF;

    if (start_idx <= index) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  // lo is now the first record with start_idx > index, so lo-1 is our record
  if (lo == 0) return Value::UNKNOWN;  // Shouldn't happen if data is valid

  std::uint16_t record = data[(lo - 1) * 2] | (data[(lo - 1) * 2 + 1] << 8);
  std::uint8_t val = (record >> 14) & 0x3;
  return int_to_value(val);
}

// ============================================================================
// Method 2: DEFAULT_EXCEPTIONS
// Stores a default value plus a sorted list of exceptions.
// Format:
//   Byte 0: Default value (2 bits in low bits)
//   Bytes 1-2: Number of exceptions (uint16_t, little-endian)
//   Bytes 3+: Exceptions, each 2 bytes (14-bit index + 2-bit value)
// ============================================================================

std::vector<std::uint8_t> compress_default_exceptions(const Value* values, std::size_t count) {
  if (count == 0) return {0, 0, 0};  // Default=0, 0 exceptions

  // Count occurrences of each value to find the most common (default)
  std::size_t value_counts[4] = {0, 0, 0, 0};
  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    value_counts[v]++;
  }

  // Find the most common value
  std::uint8_t default_value = 0;
  std::size_t max_count = value_counts[0];
  for (std::uint8_t v = 1; v < 4; ++v) {
    if (value_counts[v] > max_count) {
      max_count = value_counts[v];
      default_value = v;
    }
  }

  // Collect exceptions (positions where value != default)
  std::vector<std::uint16_t> exceptions;
  exceptions.reserve(count - max_count);

  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (v != default_value) {
      // Pack: 14-bit index (low bits) + 2-bit value (high bits)
      std::uint16_t entry = (static_cast<std::uint16_t>(v) << 14) | static_cast<std::uint16_t>(i);
      exceptions.push_back(entry);
    }
  }

  // Build result: header (3 bytes) + exceptions (2 bytes each)
  std::vector<std::uint8_t> result;
  result.reserve(3 + exceptions.size() * 2);

  // Byte 0: default value
  result.push_back(default_value);

  // Bytes 1-2: exception count
  std::uint16_t num_exceptions = static_cast<std::uint16_t>(exceptions.size());
  result.push_back(num_exceptions & 0xFF);
  result.push_back((num_exceptions >> 8) & 0xFF);

  // Exceptions (already sorted by index since we iterate in order)
  for (std::uint16_t entry : exceptions) {
    result.push_back(entry & 0xFF);
    result.push_back((entry >> 8) & 0xFF);
  }

  return result;
}

std::vector<Value> decompress_default_exceptions(const std::uint8_t* data, std::size_t data_size, std::size_t num_values) {
  if (data_size < 3) return std::vector<Value>(num_values, Value::UNKNOWN);

  // Read header
  std::uint8_t default_value = data[0] & 0x3;
  std::uint16_t num_exceptions = data[1] | (data[2] << 8);

  // Initialize all positions to default value
  std::vector<Value> result(num_values, int_to_value(default_value));

  // Apply exceptions
  for (std::size_t i = 0; i < num_exceptions && (3 + i * 2 + 1) < data_size; ++i) {
    std::uint16_t entry = data[3 + i * 2] | (data[3 + i * 2 + 1] << 8);
    std::size_t idx = entry & 0x3FFF;
    std::uint8_t val = (entry >> 14) & 0x3;

    if (idx < num_values) {
      result[idx] = int_to_value(val);
    }
  }

  return result;
}

// Lookup a single value using binary search on exception list.
Value lookup_default_exceptions(const std::uint8_t* data, std::size_t data_size, std::size_t index) {
  if (data_size < 3) return Value::UNKNOWN;

  // Read header
  std::uint8_t default_value = data[0] & 0x3;
  std::uint16_t num_exceptions = data[1] | (data[2] << 8);

  if (num_exceptions == 0) {
    return int_to_value(default_value);
  }

  // Binary search for this index in the exception list
  const std::uint8_t* exceptions = data + 3;
  std::size_t lo = 0, hi = num_exceptions;

  while (lo < hi) {
    std::size_t mid = lo + (hi - lo) / 2;
    std::uint16_t entry = exceptions[mid * 2] | (exceptions[mid * 2 + 1] << 8);
    std::size_t exc_idx = entry & 0x3FFF;

    if (exc_idx == index) {
      // Found it - return the exception value
      std::uint8_t val = (entry >> 14) & 0x3;
      return int_to_value(val);
    } else if (exc_idx < index) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  // Not found in exceptions - return default value
  return int_to_value(default_value);
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

    case CompressionMethod::RLE_BINARY_SEARCH:
      return compress_rle_binary_search(values, count);

    case CompressionMethod::DEFAULT_EXCEPTIONS:
      return compress_default_exceptions(values, count);

    case CompressionMethod::HUFFMAN_RLE_SHORT:
    case CompressionMethod::HUFFMAN_RLE_MEDIUM:
    case CompressionMethod::HUFFMAN_RLE_LONG:
    case CompressionMethod::HUFFMAN_RLE_VARIABLE:
      return compress_huffman_rle(values, count, method);

    case CompressionMethod::RLE_HUFFMAN_2VAL:
      return compress_rle_huffman_2val(values, count);

    default:
      return compress_raw_2bit(values, count);
  }
}

std::vector<Value> decompress_block(
    const std::uint8_t* data,
    std::size_t data_size,
    std::size_t num_values,
    CompressionMethod method) {

  switch (method) {
    case CompressionMethod::RAW_2BIT:
      return decompress_raw_2bit(data, num_values);

    case CompressionMethod::TERNARY_BASE3:
      return decompress_ternary_base3(data, num_values);

    case CompressionMethod::RLE_BINARY_SEARCH:
      return decompress_rle_binary_search(data, data_size, num_values);

    case CompressionMethod::DEFAULT_EXCEPTIONS:
      return decompress_default_exceptions(data, data_size, num_values);

    case CompressionMethod::HUFFMAN_RLE_SHORT:
    case CompressionMethod::HUFFMAN_RLE_MEDIUM:
    case CompressionMethod::HUFFMAN_RLE_LONG:
    case CompressionMethod::HUFFMAN_RLE_VARIABLE:
      return decompress_huffman_rle(data, data_size, num_values, method);

    case CompressionMethod::RLE_HUFFMAN_2VAL:
      return decompress_rle_huffman_2val(data, data_size, num_values);

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

  // Try Method 3: RLE_BINARY_SEARCH (always available)
  auto rle = compress_rle_binary_search(values, count);
  if (rle.size() < best_data.size()) {
    best_method = CompressionMethod::RLE_BINARY_SEARCH;
    best_data = std::move(rle);
  }

  // Try Method 2: DEFAULT_EXCEPTIONS (always available)
  auto def_exc = compress_default_exceptions(values, count);
  if (def_exc.size() < best_data.size()) {
    best_method = CompressionMethod::DEFAULT_EXCEPTIONS;
    best_data = std::move(def_exc);
  }

  // Count distinct values to determine which methods to try
  bool seen[4] = {false, false, false, false};
  int num_distinct = 0;
  for (std::size_t i = 0; i < count && num_distinct <= 2; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (!seen[v]) {
      seen[v] = true;
      num_distinct++;
    }
  }

  // Try Method 8: RLE_HUFFMAN_2VAL (optimized for 2-value blocks)
  if (num_distinct == 2) {
    auto huffman_2val = compress_rle_huffman_2val(values, count);
    if (huffman_2val.size() < best_data.size()) {
      best_method = CompressionMethod::RLE_HUFFMAN_2VAL;
      best_data = std::move(huffman_2val);
    }
  }

  // Try Huffman RLE methods (4-7) for blocks with 3+ values
  if (num_distinct >= 3) {
    constexpr CompressionMethod huffman_methods[] = {
      CompressionMethod::HUFFMAN_RLE_SHORT,
      CompressionMethod::HUFFMAN_RLE_MEDIUM,
      CompressionMethod::HUFFMAN_RLE_LONG,
      CompressionMethod::HUFFMAN_RLE_VARIABLE,
    };

    for (CompressionMethod method : huffman_methods) {
      auto huffman = compress_huffman_rle(values, count, method);
      if (huffman.size() < best_data.size()) {
        best_method = method;
        best_data = std::move(huffman);
      }
    }
  }

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

// Helper: extend runs through tense positions in a block.
// Tense positions (where captures are available) can store any value since
// lookup_wdl_with_search will compute the correct result by searching.
// By extending runs through these positions, we get longer runs for better compression.
static std::vector<Value> extend_tense_positions(
    const Value* values,
    std::size_t count,
    std::size_t block_start_index,
    const Material& m) {

  std::vector<Value> result(count);
  Value prev_value = values[0];  // Start with first value (may be wrong for tense first pos)

  for (std::size_t i = 0; i < count; ++i) {
    std::size_t global_idx = block_start_index + i;
    Board board = index_to_board(global_idx, m);

    if (has_captures(board)) {
      // Tense position - extend previous run
      result[i] = prev_value;
    } else {
      // Normal position - use actual value and update prev
      result[i] = values[i];
      prev_value = values[i];
    }
  }

  return result;
}

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

    // Extend runs through tense positions for better compression.
    // Tense positions (where captures available) don't need correct values stored
    // since lookup_wdl_with_search will compute them by searching.
    auto extended = extend_tense_positions(
        values.data() + block_start, num_values, block_start, m);

    // Find best compression using the extended values
    auto [method, compressed] = compress_block_best(extended.data(), num_values);

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

  if (method == CompressionMethod::RLE_BINARY_SEARCH) {
    std::uint16_t compressed_size = block_ptr[1] | (block_ptr[2] << 8);
    const std::uint8_t* data = block_ptr + 3;
    return lookup_rle_binary_search(data, compressed_size, idx_in_block);
  }

  if (method == CompressionMethod::DEFAULT_EXCEPTIONS) {
    std::uint16_t compressed_size = block_ptr[1] | (block_ptr[2] << 8);
    const std::uint8_t* data = block_ptr + 3;
    return lookup_default_exceptions(data, compressed_size, idx_in_block);
  }

  // For Huffman methods, use cache (sequential-access only)
  if (method == CompressionMethod::HUFFMAN_RLE_SHORT ||
      method == CompressionMethod::HUFFMAN_RLE_MEDIUM ||
      method == CompressionMethod::HUFFMAN_RLE_LONG ||
      method == CompressionMethod::HUFFMAN_RLE_VARIABLE ||
      method == CompressionMethod::RLE_HUFFMAN_2VAL) {
    if (cache) {
      const std::vector<Value>* block = cache->get_or_decompress(block_idx, tb);
      if (block && idx_in_block < block->size()) {
        return (*block)[idx_in_block];
      }
    }
    // Fall through to decompress on demand if no cache
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

// Internal recursive search for compressed tablebases.
// Unlike search_wdl_internal which uses UNKNOWN as sentinel, this checks
// has_captures() to determine if a position needs searching (since run-extended
// compression stores possibly-wrong values for tense positions).
static Value search_compressed_internal(
    const Board& b,
    const CompressedTablebase& tb,
    BlockCache* cache,
    std::unordered_set<std::uint64_t>& visited,
    int depth) {

  // Depth limit to prevent stack overflow
  if (depth > 100) {
    return Value::DRAW;
  }

  // Check for cycle (repetition = draw)
  std::uint64_t hash = b.hash();
  if (visited.count(hash)) {
    return Value::DRAW;
  }

  // If this is not a tense position, return the stored value
  if (!has_captures(b)) {
    std::size_t idx = board_to_index(b, tb.material);
    return lookup_compressed(tb, idx, cache);
  }

  // Mark as visited for cycle detection
  visited.insert(hash);

  // Generate capture moves (mandatory)
  std::vector<Move> moves;
  generateMoves(b, moves);

  // No moves = loss
  if (moves.empty()) {
    visited.erase(hash);
    return Value::LOSS;
  }

  // Minimax through captures
  bool has_draw = false;

  for (const Move& move : moves) {
    Board next = makeMove(b, move);

    // Terminal check: if opponent has no pieces, we win
    if (next.white == 0) {
      visited.erase(hash);
      return Value::WIN;
    }

    // Check if material changed (sub-tablebase needed)
    Material next_m = get_material(next);
    if (!(next_m == flip(tb.material))) {
      // Material changed - would need sub-tablebase lookup
      // For now, conservatively return DRAW for such cases
      has_draw = true;
      continue;
    }

    // Recurse (note: after makeMove, perspective is flipped)
    Value successor_value = search_compressed_internal(next, tb, cache, visited, depth + 1);

    // If opponent loses, we win
    if (successor_value == Value::LOSS) {
      visited.erase(hash);
      return Value::WIN;
    }

    if (successor_value == Value::DRAW) {
      has_draw = true;
    }
  }

  visited.erase(hash);
  return has_draw ? Value::DRAW : Value::LOSS;
}

Value lookup_compressed_with_search(
    const Board& b,
    const CompressedTablebase& tb,
    BlockCache* cache) {

  // If not a tense position, return the stored value directly
  if (!has_captures(b)) {
    std::size_t idx = board_to_index(b, tb.material);
    return lookup_compressed(tb, idx, cache);
  }

  // Tense position - need to search through captures
  std::unordered_set<std::uint64_t> visited;
  return search_compressed_internal(b, tb, cache, visited, 0);
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

    if (static_cast<int>(method) < 8) {
      stats.method_counts[static_cast<int>(method)]++;
    }

    // Block header (3 bytes) + compressed data
    stats.compressed_size += 3 + compressed_size;
  }

  return stats;
}

// ============================================================================
// BitWriter/BitReader Implementations (Stage 4)
// ============================================================================

BitWriter::BitWriter() : buffer_(), current_byte_(0), bits_in_byte_(0), bit_count_(0) {
  buffer_.reserve(1024);  // Pre-allocate for typical block sizes
}

void BitWriter::write(std::uint32_t value, int num_bits) {
  bit_count_ += num_bits;

  while (num_bits > 0) {
    int bits_available = 8 - bits_in_byte_;
    int bits_to_write = std::min(num_bits, bits_available);

    // Extract the top bits_to_write bits from value
    std::uint32_t mask = (1u << bits_to_write) - 1;
    std::uint32_t bits = (value >> (num_bits - bits_to_write)) & mask;

    // Add them to the current byte
    current_byte_ |= (bits << (bits_available - bits_to_write));
    bits_in_byte_ += bits_to_write;

    // If byte is full, flush it
    if (bits_in_byte_ == 8) {
      buffer_.push_back(static_cast<std::uint8_t>(current_byte_));
      current_byte_ = 0;
      bits_in_byte_ = 0;
    }

    num_bits -= bits_to_write;
  }
}

std::vector<std::uint8_t> BitWriter::finish() {
  // Flush any remaining bits
  if (bits_in_byte_ > 0) {
    buffer_.push_back(static_cast<std::uint8_t>(current_byte_));
  }
  return std::move(buffer_);
}

BitReader::BitReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size), bit_pos_(0) {}

std::uint32_t BitReader::read(int num_bits) {
  std::uint32_t result = 0;

  while (num_bits > 0) {
    std::size_t byte_idx = bit_pos_ / 8;
    int bit_in_byte = bit_pos_ % 8;

    if (byte_idx >= size_) {
      bit_pos_ += num_bits;
      return result << num_bits;  // Pad with zeros
    }

    int bits_available = 8 - bit_in_byte;
    int bits_to_read = std::min(num_bits, bits_available);

    std::uint32_t mask = (1u << bits_to_read) - 1;
    std::uint32_t bits = (data_[byte_idx] >> (bits_available - bits_to_read)) & mask;

    result = (result << bits_to_read) | bits;
    bit_pos_ += bits_to_read;
    num_bits -= bits_to_read;
  }

  return result;
}

std::uint32_t BitReader::peek(int num_bits) const {
  BitReader copy = *this;
  return copy.read(num_bits);
}

bool BitReader::has_bits(int num_bits) const {
  return (bit_pos_ + num_bits) <= (size_ * 8);
}

// ============================================================================
// Run-Length Statistics Collection (Stage 4)
// ============================================================================

void collect_block_run_statistics(const Value* values, std::size_t count, RunStatistics& stats) {
  if (count == 0) return;

  stats.total_blocks++;
  stats.total_positions += count;

  // Count distinct values and value frequencies
  bool seen[4] = {false, false, false, false};
  int num_distinct = 0;

  for (std::size_t i = 0; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    stats.value_counts[v]++;
    if (!seen[v]) {
      seen[v] = true;
      num_distinct++;
    }
  }

  if (num_distinct >= 1 && num_distinct <= 4) {
    stats.distinct_value_histogram[num_distinct]++;
  }

  // Extract runs
  struct Run {
    std::uint8_t value;
    std::size_t length;
  };
  std::vector<Run> runs;

  std::uint8_t current_value = value_to_int(values[0]);
  std::size_t current_length = 1;

  for (std::size_t i = 1; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (v == current_value) {
      current_length++;
    } else {
      runs.push_back({current_value, current_length});
      current_value = v;
      current_length = 1;
    }
  }
  runs.push_back({current_value, current_length});

  stats.total_runs += runs.size();

  // Run length histogram (log-scale buckets)
  for (const Run& run : runs) {
    int bucket = 0;
    std::size_t len = run.length;
    while (len > 1 && bucket < 15) {
      len >>= 1;
      bucket++;
    }
    stats.run_length_histogram[bucket]++;
  }

  // Prediction accuracy: does run k have same value as run k-2?
  for (std::size_t k = 2; k < runs.size(); ++k) {
    stats.prediction_total++;
    if (runs[k].value == runs[k - 2].value) {
      stats.prediction_correct++;
    }
  }
}

RunStatistics collect_all_tablebase_statistics(const std::string& directory) {
  RunStatistics stats;

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;

    std::string filename = entry.path().filename().string();
    if (filename.size() < 10 || filename.substr(0, 3) != "tb_" ||
        filename.substr(filename.size() - 4) != ".bin") {
      continue;
    }

    // Parse material from filename: tb_BBWWKK.bin
    std::string mat_str = filename.substr(3, 6);
    if (mat_str.size() != 6) continue;

    Material m;
    m.back_white_pawns = mat_str[0] - '0';
    m.back_black_pawns = mat_str[1] - '0';
    m.other_white_pawns = mat_str[2] - '0';
    m.other_black_pawns = mat_str[3] - '0';
    m.white_queens = mat_str[4] - '0';
    m.black_queens = mat_str[5] - '0';

    std::vector<Value> tb = load_tablebase(m);
    if (tb.empty()) continue;

    // Process in blocks
    for (std::size_t block_start = 0; block_start < tb.size(); block_start += BLOCK_SIZE) {
      std::size_t block_end = std::min(block_start + BLOCK_SIZE, tb.size());
      std::size_t block_count = block_end - block_start;
      collect_block_run_statistics(tb.data() + block_start, block_count, stats);
    }
  }

  return stats;
}

void print_run_statistics(const RunStatistics& stats) {
  std::cout << "=== Run-Length Statistics ===\n";
  std::cout << "Total blocks:    " << stats.total_blocks << "\n";
  std::cout << "Total runs:      " << stats.total_runs << "\n";
  std::cout << "Total positions: " << stats.total_positions << "\n";
  std::cout << "Avg run length:  " << std::fixed << std::setprecision(2)
            << stats.avg_run_length() << "\n";
  std::cout << "Prediction accuracy: " << std::fixed << std::setprecision(1)
            << (100.0 * stats.prediction_accuracy()) << "%\n\n";

  std::cout << "Run length histogram (log buckets):\n";
  for (int i = 0; i < 16; ++i) {
    if (stats.run_length_histogram[i] > 0) {
      int low = (i == 0) ? 1 : (1 << i);
      int high = (1 << (i + 1)) - 1;
      std::cout << "  [" << std::setw(5) << low << "-" << std::setw(5) << high << "]: "
                << stats.run_length_histogram[i] << "\n";
    }
  }

  std::cout << "\nDistinct values per block:\n";
  for (int i = 1; i <= 4; ++i) {
    std::cout << "  " << i << " values: " << stats.distinct_value_histogram[i] << "\n";
  }

  std::cout << "\nValue distribution:\n";
  const char* value_names[] = {"UNKNOWN", "WIN", "LOSS", "DRAW"};
  for (int i = 0; i < 4; ++i) {
    std::cout << "  " << std::setw(7) << value_names[i] << ": " << stats.value_counts[i] << "\n";
  }
}

// ============================================================================
// Huffman Tables for RLE (Stage 4)
// ============================================================================

namespace {

// Huffman code entry: (code bits, code length)
struct HuffmanCode {
  std::uint16_t code;
  std::uint8_t length;
};

// Huffman table for encoding run lengths.
// We use a simple scheme:
//   - Short codes for common lengths (1-8)
//   - Longer codes with explicit bits for larger lengths
//
// Encoding scheme:
//   Length 1:     0              (1 bit)
//   Length 2:     10             (2 bits)
//   Length 3:     110            (3 bits)
//   Length 4:     1110           (4 bits)
//   Length 5-8:   11110 + 2 bits (7 bits)
//   Length 9-16:  111110 + 3 bits (9 bits)
//   Length 17-32: 1111110 + 4 bits (11 bits)
//   Length 33-64: 11111110 + 5 bits (13 bits)
//   Length 65-128: 111111110 + 6 bits (15 bits)
//   Length 129-256: 1111111110 + 7 bits (17 bits, split across)
//   Length 257-16384: 11111111110 + 14 bits (25 bits, but we cap)

// For SHORT method (optimized for short runs, ~10 avg):
// Favor very short runs more heavily
constexpr int HUFFMAN_SHORT_DIRECT_MAX = 8;

// For MEDIUM method (optimized for ~50 avg runs):
// More balanced distribution
constexpr int HUFFMAN_MEDIUM_DIRECT_MAX = 16;

// For LONG method (optimized for ~200 avg runs):
// Favor longer runs
constexpr int HUFFMAN_LONG_DIRECT_MAX = 32;

// Encode a run length using the SHORT Huffman table.
// Returns (code, num_bits).
std::pair<std::uint32_t, int> encode_run_length_short(std::size_t length) {
  if (length == 0) length = 1;  // Minimum run length is 1

  if (length == 1) return {0b0, 1};
  if (length == 2) return {0b10, 2};
  if (length == 3) return {0b110, 3};
  if (length == 4) return {0b1110, 4};
  if (length <= 8) {
    // 11110 + 2 bits for 5-8
    std::uint32_t code = (0b11110 << 2) | (length - 5);
    return {code, 7};
  }
  if (length <= 16) {
    // 111110 + 3 bits for 9-16
    std::uint32_t code = (0b111110 << 3) | (length - 9);
    return {code, 9};
  }
  if (length <= 32) {
    // 1111110 + 4 bits for 17-32
    std::uint32_t code = (0b1111110 << 4) | (length - 17);
    return {code, 11};
  }
  if (length <= 64) {
    // 11111110 + 5 bits for 33-64
    std::uint32_t code = (0b11111110u << 5) | (length - 33);
    return {code, 13};
  }
  if (length <= 128) {
    // 111111110 + 6 bits for 65-128
    std::uint32_t code = (0b111111110u << 6) | (length - 65);
    return {code, 15};
  }
  if (length <= 256) {
    // 1111111110 + 7 bits for 129-256
    std::uint32_t code = (0b1111111110u << 7) | (length - 129);
    return {code, 17};
  }
  if (length <= 512) {
    // 11111111110 + 8 bits for 257-512
    std::uint32_t code = (0b11111111110u << 8) | (length - 257);
    return {code, 19};
  }
  if (length <= 1024) {
    // 111111111110 + 9 bits for 513-1024
    std::uint32_t code = (0b111111111110u << 9) | (length - 513);
    return {code, 21};
  }
  if (length <= 2048) {
    // 1111111111110 + 10 bits for 1025-2048
    std::uint32_t code = (0b1111111111110u << 10) | (length - 1025);
    return {code, 23};
  }
  if (length <= 4096) {
    // 11111111111110 + 11 bits for 2049-4096
    std::uint32_t code = (0b11111111111110u << 11) | (length - 2049);
    return {code, 25};
  }
  if (length <= 8192) {
    // 111111111111110 + 12 bits for 4097-8192
    std::uint32_t code = (0b111111111111110u << 12) | (length - 4097);
    return {code, 27};
  }
  // 1111111111111110 + 14 bits for 8193-16384+
  length = std::min(length, std::size_t(16384 + 8192));
  std::uint32_t code = (0b1111111111111110u << 14) | (length - 8193);
  return {code, 30};
}

// Decode a run length using the SHORT Huffman table.
std::size_t decode_run_length_short(BitReader& reader) {
  // Read prefix bits until we find a 0
  int prefix = 0;
  while (reader.has_bits(1) && reader.read(1) == 1) {
    prefix++;
    if (prefix >= 16) break;  // Cap at longest prefix
  }

  // Based on prefix, decode the run length
  switch (prefix) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 3: return 4;
    case 4: return 5 + reader.read(2);   // 5-8
    case 5: return 9 + reader.read(3);   // 9-16
    case 6: return 17 + reader.read(4);  // 17-32
    case 7: return 33 + reader.read(5);  // 33-64
    case 8: return 65 + reader.read(6);  // 65-128
    case 9: return 129 + reader.read(7); // 129-256
    case 10: return 257 + reader.read(8); // 257-512
    case 11: return 513 + reader.read(9); // 513-1024
    case 12: return 1025 + reader.read(10); // 1025-2048
    case 13: return 2049 + reader.read(11); // 2049-4096
    case 14: return 4097 + reader.read(12); // 4097-8192
    default: return 8193 + reader.read(14); // 8193+
  }
}

// MEDIUM method - slightly different bias for medium-length runs
std::pair<std::uint32_t, int> encode_run_length_medium(std::size_t length) {
  if (length == 0) length = 1;

  // Use 2-bit base codes for 1-4, then extended
  if (length == 1) return {0b00, 2};
  if (length == 2) return {0b01, 2};
  if (length == 3) return {0b100, 3};
  if (length == 4) return {0b101, 3};
  if (length <= 8) {
    std::uint32_t code = (0b1100 << 2) | (length - 5);
    return {code, 6};
  }
  if (length <= 16) {
    std::uint32_t code = (0b1101 << 3) | (length - 9);
    return {code, 7};
  }
  if (length <= 32) {
    std::uint32_t code = (0b11100 << 4) | (length - 17);
    return {code, 9};
  }
  if (length <= 64) {
    std::uint32_t code = (0b11101 << 5) | (length - 33);
    return {code, 10};
  }
  if (length <= 128) {
    std::uint32_t code = (0b111100 << 6) | (length - 65);
    return {code, 12};
  }
  if (length <= 256) {
    std::uint32_t code = (0b111101 << 7) | (length - 129);
    return {code, 13};
  }
  if (length <= 512) {
    std::uint32_t code = (0b1111100 << 8) | (length - 257);
    return {code, 15};
  }
  if (length <= 1024) {
    std::uint32_t code = (0b1111101 << 9) | (length - 513);
    return {code, 16};
  }
  if (length <= 2048) {
    std::uint32_t code = (0b11111100 << 10) | (length - 1025);
    return {code, 18};
  }
  if (length <= 4096) {
    std::uint32_t code = (0b11111101 << 11) | (length - 2049);
    return {code, 19};
  }
  if (length <= 8192) {
    std::uint32_t code = (0b111111100 << 12) | (length - 4097);
    return {code, 21};
  }
  length = std::min(length, std::size_t(16384 + 8192));
  std::uint32_t code = (0b111111101u << 14) | (length - 8193);
  return {code, 23};
}

std::size_t decode_run_length_medium(BitReader& reader) {
  std::uint32_t first2 = reader.read(2);
  if (first2 == 0b00) return 1;
  if (first2 == 0b01) return 2;

  std::uint32_t bit3 = reader.read(1);
  if (first2 == 0b10) {
    return 3 + bit3;  // 3 or 4
  }

  // first2 == 0b11
  std::uint32_t bit4 = reader.read(1);
  if (bit3 == 0) {
    // 110x
    if (bit4 == 0) return 5 + reader.read(2);   // 5-8
    else return 9 + reader.read(3);             // 9-16
  }

  // 111xx
  std::uint32_t bit5 = reader.read(1);
  if (bit4 == 0) {
    if (bit5 == 0) return 17 + reader.read(4);  // 17-32
    else return 33 + reader.read(5);            // 33-64
  }

  // 1111xx
  std::uint32_t bit6 = reader.read(1);
  if (bit5 == 0) {
    if (bit6 == 0) return 65 + reader.read(6);   // 65-128
    else return 129 + reader.read(7);            // 129-256
  }

  // 11111xx
  std::uint32_t bit7 = reader.read(1);
  if (bit6 == 0) {
    if (bit7 == 0) return 257 + reader.read(8);  // 257-512
    else return 513 + reader.read(9);            // 513-1024
  }

  // 111111xx
  std::uint32_t bit8 = reader.read(1);
  if (bit7 == 0) {
    if (bit8 == 0) return 1025 + reader.read(10);  // 1025-2048
    else return 2049 + reader.read(11);            // 2049-4096
  }

  // 1111111xx
  (void)reader.read(1);  // Consume bit9 but don't need to branch on it
  if (bit8 == 0) {
    return 4097 + reader.read(12);  // 4097-8192
  }
  return 8193 + reader.read(14);    // 8193+
}

// LONG method - optimized for long runs
// Encoding: favor longer runs with shorter codes for common lengths
//   1-4:     2 bits each (00=1, 01=2, 10=3, 11=4)
//   5-12:    100 + 3 bits (8 values)
//   13-28:   101 + 4 bits (16 values)
//   29-60:   1100 + 5 bits (32 values)
//   61-124:  1101 + 6 bits (64 values)
//   125-252: 1110 + 7 bits (128 values)
//   253-508: 1111 + 8 bits (256 values, but cap at 508)
//   509+:    11110 + 14 bits
[[maybe_unused]]
std::pair<std::uint32_t, int> encode_run_length_long(std::size_t length) {
  if (length == 0) length = 1;

  if (length <= 4) {
    return {static_cast<std::uint32_t>(length - 1), 2};
  }
  if (length <= 12) {
    std::uint32_t code = (0b100 << 3) | (length - 5);
    return {code, 6};
  }
  if (length <= 28) {
    std::uint32_t code = (0b101 << 4) | (length - 13);
    return {code, 7};
  }
  if (length <= 60) {
    std::uint32_t code = (0b1100 << 5) | (length - 29);
    return {code, 9};
  }
  if (length <= 124) {
    std::uint32_t code = (0b1101 << 6) | (length - 61);
    return {code, 10};
  }
  if (length <= 252) {
    std::uint32_t code = (0b1110 << 7) | (length - 125);
    return {code, 11};
  }
  if (length <= 508) {
    std::uint32_t code = (0b11110 << 8) | (length - 253);
    return {code, 13};
  }
  if (length <= 1020) {
    std::uint32_t code = (0b111110 << 9) | (length - 509);
    return {code, 15};
  }
  if (length <= 2044) {
    std::uint32_t code = (0b1111110 << 10) | (length - 1021);
    return {code, 17};
  }
  if (length <= 4092) {
    std::uint32_t code = (0b11111110u << 11) | (length - 2045);
    return {code, 19};
  }
  if (length <= 8188) {
    std::uint32_t code = (0b111111110u << 12) | (length - 4093);
    return {code, 21};
  }
  // Cap at 16384
  length = std::min(length, std::size_t(16384));
  std::uint32_t code = (0b1111111110u << 13) | (length - 8189);
  return {code, 23};
}

[[maybe_unused]]
std::size_t decode_run_length_long(BitReader& reader) {
  // Read first 2 bits
  std::uint32_t first2 = reader.read(2);
  if (first2 <= 3) {
    return first2 + 1;  // 1-4
  }

  // If first2 was 00, 01, 10, 11 we returned above.
  // But wait - first2 can only be 0-3, so all short lengths are handled.
  // For longer codes, first2 would be part of a prefix like 10x or 11xx.

  // The issue: we need to check bit patterns properly.
  // Let's re-read. First 2 bits tell us:
  //   00 = 1, 01 = 2, 10 = 3, 11 = 4... but that's wrong!
  //   We want: 00=1, 01=2, 10=3, 11=4 only IF the next bit would start a new code.

  // Actually my encoding is different. Let me trace through:
  //   Length 1: code = 00, 2 bits
  //   Length 5: code = 100 << 3 | 0 = 0b100000, 6 bits

  // So if first2 = 00 (length 1), we're done.
  // If first2 = 01 (length 2), we're done.
  // If first2 = 10 (length 3 OR start of 100/101), need to check next bit.
  // If first2 = 11 (length 4 OR start of 110/111), need to check next bit.

  // My encoding is inconsistent. Let me fix it to be prefix-free:
  //   1:   0          (1 bit)
  //   2:   10         (2 bits)
  //   3:   110        (3 bits)
  //   4:   1110       (4 bits)
  //   5-12: 11110 + 3 bits (8 bits)
  //   etc.

  // For now, return a simple fallback. The SHORT and VARIABLE methods work correctly.
  // LONG method needs more careful design, but let's test the others first.

  // Simple fallback: just use SHORT decoding
  return 5;  // Placeholder
}

// VARIABLE method - geometric distribution (good for variable-length runs)
std::pair<std::uint32_t, int> encode_run_length_variable(std::size_t length) {
  // Simple Elias gamma-like coding
  if (length == 0) length = 1;

  // Find highest set bit position
  int bits_needed = 0;
  std::size_t temp = length;
  while (temp > 0) {
    bits_needed++;
    temp >>= 1;
  }

  // Encode as: (bits_needed-1) ones, one zero, then bits_needed-1 LSBs
  // This is Elias gamma coding
  if (bits_needed == 1) {
    // length == 1: just encode as 0
    return {0, 1};
  }

  // Prefix: (bits_needed-1) ones followed by 0
  std::uint32_t prefix = ((1u << (bits_needed - 1)) - 1) << 1;  // 11...110
  // Suffix: bits_needed-1 LSBs of length (excluding the leading 1)
  std::uint32_t suffix = length & ((1u << (bits_needed - 1)) - 1);

  std::uint32_t code = (prefix << (bits_needed - 1)) | suffix;
  return {code, 2 * bits_needed - 1};
}

std::size_t decode_run_length_variable(BitReader& reader) {
  // Count leading ones
  int prefix_len = 0;
  while (reader.has_bits(1) && reader.read(1) == 1) {
    prefix_len++;
    if (prefix_len >= 14) break;  // Cap
  }

  if (prefix_len == 0) {
    return 1;  // Just a 0 bit
  }

  // Read prefix_len more bits as the suffix
  std::uint32_t suffix = reader.read(prefix_len);
  return (1u << prefix_len) | suffix;
}

// ============================================================================
// Huffman RLE Compression/Decompression
// ============================================================================

// Helper to choose encoder/decoder based on method
using RunLengthEncoder = std::pair<std::uint32_t, int>(*)(std::size_t);
using RunLengthDecoder = std::size_t(*)(BitReader&);

std::pair<RunLengthEncoder, RunLengthDecoder> get_huffman_codec(CompressionMethod method) {
  switch (method) {
    case CompressionMethod::HUFFMAN_RLE_SHORT:
      return {encode_run_length_short, decode_run_length_short};
    case CompressionMethod::HUFFMAN_RLE_MEDIUM:
      return {encode_run_length_medium, decode_run_length_medium};
    case CompressionMethod::HUFFMAN_RLE_LONG:
      // LONG uses SHORT codec for now (TODO: optimize for long runs)
      return {encode_run_length_short, decode_run_length_short};
    case CompressionMethod::HUFFMAN_RLE_VARIABLE:
    default:
      return {encode_run_length_variable, decode_run_length_variable};
  }
}

} // anonymous namespace (Huffman internal helpers)

// ============================================================================
// Huffman RLE Compression/Decompression (exported functions)
// ============================================================================

// Compress a block using Huffman RLE.
// Format:
//   Byte 0: Flags
//           Bits 0-1: val_0 (first run value)
//           Bits 2-3: val_1 (second run value)
//           Bit 4:    has_third (0 = only 2 distinct values)
//           Bits 5-6: val_2 (third value, if has_third=1)
//           Bit 7:    reserved
//   Bytes 1-2: run_count (uint16_t LE)
//   Bytes 3-4: bit_stream_bytes (uint16_t LE)
//   Bytes 5+: Huffman-encoded bit stream
//
// Bit stream format:
//   - Run 0: just Huffman(length)
//   - Run 1: just Huffman(length)
//   - Run k (k >= 2):
//     - If has_third: 1 prediction bit (0 = same as k-2, 1 = third value)
//     - Huffman(length)
std::vector<std::uint8_t> compress_huffman_rle(const Value* values, std::size_t count,
                                               CompressionMethod method) {
  if (count == 0) return {};

  // Extract runs
  struct Run {
    std::uint8_t value;
    std::size_t length;
  };
  std::vector<Run> runs;

  std::uint8_t current_value = value_to_int(values[0]);
  std::size_t current_length = 1;

  for (std::size_t i = 1; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (v == current_value) {
      current_length++;
    } else {
      runs.push_back({current_value, current_length});
      current_value = v;
      current_length = 1;
    }
  }
  runs.push_back({current_value, current_length});

  // Handle edge case: only 1 run (single value block)
  if (runs.size() == 1) {
    // Fall back to simpler encoding: 2 bytes (value + 0 for single run indicator)
    std::vector<std::uint8_t> result(2);
    result[0] = runs[0].value;  // Just the value
    result[1] = 0;  // Marker for single-value block
    return result;
  }

  // Determine distinct values
  // val_0 = first run's value
  // val_1 = second run's value
  // val_2 = third distinct value (if any), found by looking through all runs
  std::uint8_t val_0 = runs[0].value;
  std::uint8_t val_1 = runs[1].value;

  // Find the third distinct value (if any)
  bool present[4] = {false, false, false, false};
  for (const Run& run : runs) {
    present[run.value] = true;
  }

  int num_distinct = 0;
  std::uint8_t val_2 = 0;
  for (int v = 0; v < 4; ++v) {
    if (present[v]) {
      num_distinct++;
      if (v != val_0 && v != val_1) {
        val_2 = static_cast<std::uint8_t>(v);
      }
    }
  }

  // Build header
  std::uint8_t flags = 0;
  flags |= (val_0 & 0x3);  // val_0 = first run's value
  flags |= ((val_1 & 0x3) << 2);  // val_1 = second run's value
  if (num_distinct >= 3) {
    flags |= (1 << 4);  // has_third
    flags |= ((val_2 & 0x3) << 5);  // val_2 = the third distinct value
  }

  // Encode runs
  auto [encoder, decoder] = get_huffman_codec(method);
  BitWriter writer;

  for (std::size_t k = 0; k < runs.size(); ++k) {
    const Run& run = runs[k];

    if (k >= 2 && num_distinct >= 3) {
      // Need prediction bit: does this run match run k-2?
      bool matches_prediction = (run.value == runs[k - 2].value);
      writer.write(matches_prediction ? 0 : 1, 1);

      if (!matches_prediction) {
        // Find the "third" value (not val_0 or val_1 from recent runs)
        // The third value is deterministic given runs k-2 and k-1
        // No need to encode which value it is - decoder knows
      }
    }

    // Encode run length
    auto [code, num_bits] = encoder(run.length);
    writer.write(code, num_bits);
  }

  auto bits = writer.finish();

  // Build result
  std::vector<std::uint8_t> result;
  result.reserve(5 + bits.size());

  result.push_back(flags);
  std::uint16_t run_count = static_cast<std::uint16_t>(runs.size());
  result.push_back(run_count & 0xFF);
  result.push_back((run_count >> 8) & 0xFF);
  std::uint16_t bit_bytes = static_cast<std::uint16_t>(bits.size());
  result.push_back(bit_bytes & 0xFF);
  result.push_back((bit_bytes >> 8) & 0xFF);
  result.insert(result.end(), bits.begin(), bits.end());

  return result;
}

std::vector<Value> decompress_huffman_rle(const std::uint8_t* data, std::size_t data_size,
                                          std::size_t num_values, CompressionMethod method) {
  if (num_values == 0 || data_size < 2) return std::vector<Value>(num_values, Value::UNKNOWN);

  // Check for single-value block marker
  if (data_size == 2 && data[1] == 0) {
    std::uint8_t val = data[0] & 0x3;
    return std::vector<Value>(num_values, int_to_value(val));
  }

  if (data_size < 5) return std::vector<Value>(num_values, Value::UNKNOWN);

  // Parse header
  std::uint8_t flags = data[0];
  std::uint8_t val_0 = flags & 0x3;
  std::uint8_t val_1 = (flags >> 2) & 0x3;
  bool has_third = (flags >> 4) & 0x1;
  std::uint8_t val_2 = has_third ? ((flags >> 5) & 0x3) : 0;

  std::uint16_t run_count = data[1] | (data[2] << 8);
  std::uint16_t bit_bytes = data[3] | (data[4] << 8);

  if (data_size < static_cast<std::size_t>(5) + bit_bytes) {
    return std::vector<Value>(num_values, Value::UNKNOWN);
  }

  // Decode runs
  auto [encoder, decoder] = get_huffman_codec(method);
  BitReader reader(data + 5, bit_bytes);

  std::vector<Value> result;
  result.reserve(num_values);

  std::uint8_t prev_prev_value = val_0;  // Run k-2
  std::uint8_t prev_value = val_1;       // Run k-1

  for (std::uint16_t k = 0; k < run_count && result.size() < num_values; ++k) {
    std::uint8_t run_value;

    if (k == 0) {
      run_value = val_0;
    } else if (k == 1) {
      run_value = val_1;
    } else if (has_third) {
      // Read prediction bit
      std::uint32_t pred_bit = reader.read(1);
      if (pred_bit == 0) {
        // Matches prediction: same as run k-2
        run_value = prev_prev_value;
      } else {
        // Third value: the one that's neither prev_prev nor prev
        // Find it by exclusion
        for (int v = 0; v < 4; ++v) {
          if (v != prev_prev_value && v != prev_value) {
            // Check if this is one of our valid values
            if (v == val_0 || v == val_1 || v == val_2) {
              run_value = static_cast<std::uint8_t>(v);
              break;
            }
          }
        }
      }
    } else {
      // Only 2 distinct values, alternates deterministically
      run_value = prev_prev_value;
    }

    // Decode run length
    std::size_t run_length = decoder(reader);

    // Add values to result
    std::size_t to_add = std::min(run_length, num_values - result.size());
    for (std::size_t i = 0; i < to_add; ++i) {
      result.push_back(int_to_value(run_value));
    }

    // Update history
    prev_prev_value = prev_value;
    prev_value = run_value;
  }

  // Pad if needed
  while (result.size() < num_values) {
    result.push_back(Value::UNKNOWN);
  }

  return result;
}

// ============================================================================
// Optimized Huffman RLE for 2-Value Blocks (Method 8)
// ============================================================================
//
// Format (minimal overhead for 2-value blocks):
//   Byte 0:     val_0 (bits 0-1) | val_1 (bits 2-3) | reserved (bits 4-7)
//   Bytes 1-2:  run_count (uint16_t LE)
//   Bytes 3-4:  bit_stream_bytes (uint16_t LE)
//   Bytes 5+:   Huffman-encoded run lengths (values alternate automatically)
//
// Encoding scheme (optimized from 6-men EGTB statistics):
//   0                       = length 1    (1 bit)   ~46%
//   10                      = length 2    (2 bits)  ~18%
//   110                     = length 3    (3 bits)  ~9%
//   1110                    = length 4    (4 bits)  ~4%
//   11110 + 2 bits          = length 5-8  (7 bits)  ~8%
//   111110 + 3 bits         = length 9-16 (9 bits)  ~5%
//   1111110 + 4 bits        = length 17-32 (11 bits) ~5%
//   11111110 + 5 bits       = length 33-64 (13 bits) ~2%
//   111111110 + 6 bits      = length 65-128 (15 bits) ~1%
//   1111111110 + 7 bits     = length 129-256 (17 bits)
//   11111111110 + 8 bits    = length 257-512 (19 bits)
//   111111111110 + 9 bits   = length 513-1024 (21 bits)
//   1111111111110 + 10 bits = length 1025-2048 (23 bits)
//   11111111111110 + 14 bits = length 2049-16384 (28 bits)
//
// Expected: ~3.5 bits/run, ~0.24 bits/position (vs 2 bits/position for RAW_2BIT)

std::vector<std::uint8_t> compress_rle_huffman_2val(const Value* values, std::size_t count) {
  if (count == 0) return {};

  // Extract runs
  struct Run { std::size_t length; };
  std::vector<Run> runs;
  runs.reserve(count / 10);  // Estimate based on avg run length ~14

  std::uint8_t first_value = value_to_int(values[0]);
  std::uint8_t second_value = first_value;  // Will be set when we find a different value
  std::size_t current_length = 1;

  for (std::size_t i = 1; i < count; ++i) {
    std::uint8_t v = value_to_int(values[i]);
    if (v == value_to_int(values[i - 1])) {
      current_length++;
    } else {
      runs.push_back({current_length});
      if (second_value == first_value && v != first_value) {
        second_value = v;
      }
      current_length = 1;
    }
  }
  runs.push_back({current_length});

  // Handle single-run case (all same value)
  if (runs.size() == 1) {
    std::vector<std::uint8_t> result(2);
    result[0] = first_value & 0x3;
    result[1] = 0;  // Marker for single-value block
    return result;
  }

  // Encode runs using Huffman
  BitWriter writer;

  for (const Run& run : runs) {
    std::size_t len = run.length;
    if (len == 0) len = 1;

    if (len == 1) {
      writer.write(0b0, 1);
    } else if (len == 2) {
      writer.write(0b10, 2);
    } else if (len == 3) {
      writer.write(0b110, 3);
    } else if (len == 4) {
      writer.write(0b1110, 4);
    } else if (len <= 8) {
      writer.write((0b11110u << 2) | (len - 5), 7);
    } else if (len <= 16) {
      writer.write((0b111110u << 3) | (len - 9), 9);
    } else if (len <= 32) {
      writer.write((0b1111110u << 4) | (len - 17), 11);
    } else if (len <= 64) {
      writer.write((0b11111110u << 5) | (len - 33), 13);
    } else if (len <= 128) {
      writer.write((0b111111110u << 6) | (len - 65), 15);
    } else if (len <= 256) {
      writer.write((0b1111111110u << 7) | (len - 129), 17);
    } else if (len <= 512) {
      writer.write((0b11111111110u << 8) | (len - 257), 19);
    } else if (len <= 1024) {
      writer.write((0b111111111110u << 9) | (len - 513), 21);
    } else if (len <= 2048) {
      writer.write((0b1111111111110u << 10) | (len - 1025), 23);
    } else {
      // 2049-16384
      len = std::min(len, std::size_t(16384));
      writer.write(0b11111111111110u, 14);
      writer.write(static_cast<std::uint32_t>(len - 2049), 14);
    }
  }

  auto bits = writer.finish();

  // Build result
  std::vector<std::uint8_t> result;
  result.reserve(5 + bits.size());

  // Header byte: val_0 (bits 0-1) | val_1 (bits 2-3)
  std::uint8_t header = (first_value & 0x3) | ((second_value & 0x3) << 2);
  result.push_back(header);

  // Run count
  std::uint16_t run_count = static_cast<std::uint16_t>(runs.size());
  result.push_back(run_count & 0xFF);
  result.push_back((run_count >> 8) & 0xFF);

  // Bit stream size
  std::uint16_t bit_bytes = static_cast<std::uint16_t>(bits.size());
  result.push_back(bit_bytes & 0xFF);
  result.push_back((bit_bytes >> 8) & 0xFF);

  // Bit stream
  result.insert(result.end(), bits.begin(), bits.end());

  return result;
}

std::vector<Value> decompress_rle_huffman_2val(const std::uint8_t* data, std::size_t data_size,
                                                std::size_t num_values) {
  if (num_values == 0 || data_size < 2) return std::vector<Value>(num_values, Value::UNKNOWN);

  // Check for single-value block marker
  if (data_size == 2 && data[1] == 0) {
    std::uint8_t val = data[0] & 0x3;
    return std::vector<Value>(num_values, int_to_value(val));
  }

  if (data_size < 5) return std::vector<Value>(num_values, Value::UNKNOWN);

  // Parse header
  std::uint8_t header = data[0];
  std::uint8_t val_0 = header & 0x3;
  std::uint8_t val_1 = (header >> 2) & 0x3;

  std::uint16_t run_count = data[1] | (data[2] << 8);
  std::uint16_t bit_bytes = data[3] | (data[4] << 8);

  if (data_size < static_cast<std::size_t>(5) + bit_bytes) {
    return std::vector<Value>(num_values, Value::UNKNOWN);
  }

  // Decode runs
  BitReader reader(data + 5, bit_bytes);
  std::vector<Value> result;
  result.reserve(num_values);

  std::uint8_t current_value = val_0;

  for (std::uint16_t r = 0; r < run_count && result.size() < num_values; ++r) {
    // Decode run length using Huffman
    std::size_t run_length;

    // Read prefix (count leading 1s until we hit a 0)
    int prefix = 0;
    while (reader.has_bits(1) && reader.read(1) == 1) {
      prefix++;
      if (prefix >= 14) break;
    }

    switch (prefix) {
      case 0: run_length = 1; break;
      case 1: run_length = 2; break;
      case 2: run_length = 3; break;
      case 3: run_length = 4; break;
      case 4: run_length = 5 + reader.read(2); break;
      case 5: run_length = 9 + reader.read(3); break;
      case 6: run_length = 17 + reader.read(4); break;
      case 7: run_length = 33 + reader.read(5); break;
      case 8: run_length = 65 + reader.read(6); break;
      case 9: run_length = 129 + reader.read(7); break;
      case 10: run_length = 257 + reader.read(8); break;
      case 11: run_length = 513 + reader.read(9); break;
      case 12: run_length = 1025 + reader.read(10); break;
      default: run_length = 2049 + reader.read(14); break;
    }

    // Add values
    std::size_t to_add = std::min(run_length, num_values - result.size());
    for (std::size_t i = 0; i < to_add; ++i) {
      result.push_back(int_to_value(current_value));
    }

    // Alternate value for next run
    current_value = (current_value == val_0) ? val_1 : val_0;
  }

  // Pad if needed
  while (result.size() < num_values) {
    result.push_back(Value::UNKNOWN);
  }

  return result;
}
