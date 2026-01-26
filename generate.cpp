#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include "compression.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <map>
#include <tuple>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <omp.h>

namespace {

// Statistics for a generation run
struct Stats {
  std::size_t total_positions = 0;
  std::size_t wins = 0;
  std::size_t losses = 0;
  std::size_t draws = 0;
  std::size_t with_captures = 0;  // Positions where captures are forced
};

// ============================================================================
// Verification Functions - Stop at first error with debug info
// ============================================================================

// Print detailed position info for debugging
[[maybe_unused]]
void print_position_debug(const Board& board, std::size_t idx, const Material& m,
                          Value stored, Value computed) {
  std::cerr << "\n=== VERIFICATION ERROR ===\n";
  std::cerr << "Material: " << m << "\n";
  std::cerr << "Index: " << idx << "\n";
  std::cerr << "Stored: " << static_cast<int>(stored)
            << " (" << (stored == Value::WIN ? "WIN" :
                       stored == Value::LOSS ? "LOSS" :
                       stored == Value::DRAW ? "DRAW" : "UNKNOWN") << ")\n";
  std::cerr << "Computed: " << static_cast<int>(computed)
            << " (" << (computed == Value::WIN ? "WIN" :
                       computed == Value::LOSS ? "LOSS" :
                       computed == Value::DRAW ? "DRAW" : "UNKNOWN") << ")\n";
  std::cerr << "has_captures: " << has_captures(board) << "\n";
  std::cerr << "Board:\n" << board << "\n";

  // Show successors
  std::vector<Move> moves;
  generateMoves(board, moves);
  std::cerr << "Moves (" << moves.size() << "):\n";
  for (size_t i = 0; i < moves.size(); i++) {
    Board next = makeMove(board, moves[i]);
    Material nm = get_material(next);
    std::cerr << "  " << i << ": captures=0x" << std::hex << moves[i].captures
              << std::dec << " -> " << nm << "\n";
  }
}

// Verify a generated tablebase against its successors
// Returns true if verification passes
bool verify_generated_tablebase(
    const std::vector<Value>& table,
    const Material& m,
    const std::vector<Value>& table_fm,  // flip(m)'s table (empty if symmetric)
    CompressedTablebaseManager* manager) {

  std::size_t size = table.size();
  Material fm = flip(m);
  bool symmetric = (m == fm);

  std::atomic<bool> error_found{false};
  std::atomic<std::size_t> error_idx{0};

  #pragma omp parallel for schedule(dynamic, 1024)
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (error_found.load(std::memory_order_relaxed)) continue;

    Board board = index_to_board(idx, m);
    Value stored = table[idx];

    // Compute expected value
    std::vector<Move> moves;
    generateMoves(board, moves);

    if (moves.empty()) {
      if (stored != Value::LOSS) {
        bool expected = false;
        if (error_found.compare_exchange_strong(expected, true)) {
          error_idx.store(idx);
        }
      }
      continue;
    }

    bool found_loss = false;
    bool all_wins = true;

    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      Material next_m = get_material(next);

      Value succ_val = Value::UNKNOWN;

      if (next_m.white_pieces() == 0) {
        succ_val = Value::LOSS;
      } else if (next_m.black_pieces() == 0) {
        succ_val = Value::WIN;
      } else if (next_m == fm) {
        std::size_t next_idx = board_to_index(next, fm);
        succ_val = symmetric ? table[next_idx] : table_fm[next_idx];
      } else {
        succ_val = manager->lookup_wdl(next);
      }

      if (succ_val == Value::LOSS) found_loss = true;
      if (succ_val != Value::WIN) all_wins = false;
    }

    Value expected;
    if (found_loss) expected = Value::WIN;
    else if (all_wins) expected = Value::LOSS;
    else expected = Value::DRAW;

    if (stored != expected) {
      bool was_false = false;
      if (error_found.compare_exchange_strong(was_false, true)) {
        error_idx.store(idx);
      }
    }
  }

  if (error_found.load()) {
    std::size_t idx = error_idx.load();
    Board board = index_to_board(idx, m);
    Value stored = table[idx];

    // Recompute for debug output
    std::vector<Move> moves;
    generateMoves(board, moves);

    bool found_loss = false;
    bool all_wins = true;
    bool has_draw = false;

    std::cerr << "\n=== GENERATION VERIFICATION ERROR ===\n";
    std::cerr << "Material: " << m << "\n";
    std::cerr << "Index: " << idx << "\n";
    std::cerr << "Stored: " << static_cast<int>(stored) << "\n";
    std::cerr << "has_captures: " << has_captures(board) << "\n";
    std::cerr << "Board:\n" << board << "\n";
    std::cerr << "Successors:\n";

    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      Material next_m = get_material(next);

      Value succ_val = Value::UNKNOWN;
      std::string source;

      if (next_m.white_pieces() == 0) {
        succ_val = Value::LOSS;
        source = "terminal";
      } else if (next_m.black_pieces() == 0) {
        succ_val = Value::WIN;
        source = "terminal";
      } else if (next_m == fm) {
        std::size_t next_idx = board_to_index(next, fm);
        succ_val = symmetric ? table[next_idx] : table_fm[next_idx];
        source = "table_fm[" + std::to_string(next_idx) + "]";
      } else {
        succ_val = manager->lookup_wdl(next);
        std::ostringstream oss;
        oss << "manager(" << next_m << ")";
        source = oss.str();
      }

      std::cerr << "  -> " << next_m << " val=" << static_cast<int>(succ_val)
                << " (" << source << ")\n";

      if (succ_val == Value::LOSS) found_loss = true;
      if (succ_val != Value::WIN) all_wins = false;
      if (succ_val == Value::DRAW) has_draw = true;
    }

    Value expected;
    if (found_loss) expected = Value::WIN;
    else if (all_wins) expected = Value::LOSS;
    else expected = Value::DRAW;

    std::cerr << "Expected: " << static_cast<int>(expected) << "\n";
    std::cerr << "found_loss=" << found_loss << " all_wins=" << all_wins
              << " has_draw=" << has_draw << "\n";

    return false;
  }

  return true;
}

// Verify compression roundtrip for all methods
bool verify_compression_roundtrip(
    const std::vector<Value>& original,
    const Material& m) {

  std::size_t size = original.size();

  // Mark don't-care positions
  CompressionStats cstats;
  std::vector<Value> marked = mark_dont_care_positions(original, m, cstats);

  // Test block by block (compression methods are designed for BLOCK_SIZE chunks)
  for (std::size_t block_start = 0; block_start < size; block_start += BLOCK_SIZE) {
    std::size_t block_end = std::min(block_start + BLOCK_SIZE, size);
    std::size_t block_size = block_end - block_start;

    // Count distinct values in this block to determine which methods are valid
    bool seen[4] = {false, false, false, false};
    int num_distinct = 0;
    for (std::size_t i = 0; i < block_size && num_distinct <= 3; ++i) {
      std::uint8_t v = static_cast<std::uint8_t>(marked[block_start + i]);
      if (!seen[v]) {
        seen[v] = true;
        num_distinct++;
      }
    }

    // Test each applicable compression method for this block
    struct MethodInfo {
      CompressionMethod method;
      const char* name;
      int required_distinct;  // 0 = any, 2 = exactly 2, 3 = exactly 3
    };

    MethodInfo methods[] = {
      {CompressionMethod::RAW_2BIT, "RAW_2BIT", 0},
      {CompressionMethod::RLE_BINARY_SEARCH, "RLE_BINARY_SEARCH", 0},
      {CompressionMethod::DEFAULT_EXCEPTIONS, "DEFAULT_EXCEPTIONS", 0},
      {CompressionMethod::RLE_HUFFMAN_2VAL, "RLE_HUFFMAN_2VAL", 2},
      {CompressionMethod::RLE_HUFFMAN_3VAL, "RLE_HUFFMAN_3VAL", 3},
    };

    for (const auto& mi : methods) {
      // Skip methods that don't apply to this block's value count
      if (mi.required_distinct != 0 && mi.required_distinct != num_distinct) {
        continue;
      }

      CompressionMethod method = mi.method;

      // Compress this block
      auto compressed = compress_block(marked.data() + block_start, block_size, method);

      // Decompress
      auto decompressed = decompress_block(compressed.data(), compressed.size(),
                                            block_size, method);

      // Verify roundtrip for quiet positions only (don't-care positions don't matter)
      for (std::size_t i = 0; i < block_size; i++) {
        std::size_t idx = block_start + i;
        Board board = index_to_board(idx, m);

        // Skip don't-care positions - their stored value doesn't matter
        if (has_captures(board)) continue;

        if (marked[idx] != decompressed[i]) {
          std::cerr << "\n=== COMPRESSION ROUNDTRIP ERROR ===\n";
          std::cerr << "Method: " << mi.name << "\n";
          std::cerr << "Material: " << m << "\n";
          std::cerr << "Block: " << (block_start / BLOCK_SIZE) << " (start=" << block_start << ")\n";
          std::cerr << "Index in block: " << i << "\n";
          std::cerr << "Global index: " << idx << "\n";
          std::cerr << "Original (marked): " << static_cast<int>(marked[idx]) << "\n";
          std::cerr << "Decompressed: " << static_cast<int>(decompressed[i]) << "\n";
          std::cerr << "Board:\n" << board << "\n";
          return false;
        }
      }
    }
  }

  return true;
}

// Verify compressed lookup matches original for quiet positions
bool verify_compressed_lookup(
    const std::vector<Value>& original,
    const CompressedTablebase& ctb,
    const Material& m) {

  std::size_t size = original.size();

  std::atomic<bool> error_found{false};
  std::atomic<std::size_t> error_idx{0};

  #pragma omp parallel for schedule(dynamic, 1024)
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (error_found.load(std::memory_order_relaxed)) continue;

    Board board = index_to_board(idx, m);

    // Skip don't-care positions
    if (has_captures(board)) continue;

    Value orig = original[idx];
    Value compressed = lookup_compressed(ctb, idx);

    if (orig != compressed) {
      bool expected = false;
      if (error_found.compare_exchange_strong(expected, true)) {
        error_idx.store(idx);
      }
    }
  }

  if (error_found.load()) {
    std::size_t idx = error_idx.load();
    Board board = index_to_board(idx, m);
    Value orig = original[idx];
    Value compressed = lookup_compressed(ctb, idx);

    std::cerr << "\n=== COMPRESSED LOOKUP ERROR ===\n";
    std::cerr << "Material: " << m << "\n";
    std::cerr << "Index: " << idx << "\n";
    std::cerr << "Original: " << static_cast<int>(orig) << "\n";
    std::cerr << "Compressed lookup: " << static_cast<int>(compressed) << "\n";
    std::cerr << "has_captures: " << has_captures(board) << "\n";
    std::cerr << "Board:\n" << board << "\n";
    return false;
  }

  return true;
}

// ============================================================================
// Global State for Parallel Generation
// ============================================================================

// Global tablebase storage (protected by mutex for writes)
std::unordered_map<Material, std::vector<Value>> g_wdl_tablebases;
std::mutex g_tablebases_mutex;

// Thread-safe read: returns a copy of the tablebase (or empty if not found)
std::vector<Value> get_wdl_tablebase(const Material& m) {
  std::lock_guard<std::mutex> lock(g_tablebases_mutex);
  auto it = g_wdl_tablebases.find(m);
  return (it != g_wdl_tablebases.end()) ? it->second : std::vector<Value>{};
}

// Thread-safe write: stores tablebase
void store_wdl_tablebase(const Material& m, std::vector<Value>&& table) {
  std::lock_guard<std::mutex> lock(g_tablebases_mutex);
  g_wdl_tablebases[m] = std::move(table);
}

// Global DTM tablebases (thread-safe access)
std::unordered_map<Material, std::vector<DTM>> g_dtm_tablebases;
std::mutex g_dtm_tablebases_mutex;

std::vector<DTM> get_dtm_tablebase(const Material& m) {
  std::lock_guard<std::mutex> lock(g_dtm_tablebases_mutex);
  auto it = g_dtm_tablebases.find(m);
  return (it != g_dtm_tablebases.end()) ? it->second : std::vector<DTM>{};
}

void store_dtm_tablebase(const Material& m, std::vector<DTM>&& table) {
  std::lock_guard<std::mutex> lock(g_dtm_tablebases_mutex);
  g_dtm_tablebases[m] = std::move(table);
}

// Forward declarations for parallel solver
std::vector<Material> all_materials(int max_pieces);
std::vector<DTM> generate_dtm(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
    const std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases);

// has_captures() is now in movegen.h

// Get all materials reachable by captures from the current material
std::vector<Material> capture_targets(const Material& m) {
  std::vector<Material> result;

  // Capture removes one opponent piece
  if (m.back_black_pawns > 0) {
    Material next = m;
    next.back_black_pawns--;
    result.push_back(next);
  }
  if (m.other_black_pawns > 0) {
    Material next = m;
    next.other_black_pawns--;
    result.push_back(next);
  }
  if (m.black_queens > 0) {
    Material next = m;
    next.black_queens--;
    result.push_back(next);
  }

  return result;
}

// Get all materials reachable by pawn advancement (back pawn -> other pawn)
std::vector<Material> advancement_targets(const Material& m) {
  std::vector<Material> result;

  if (m.back_white_pawns > 0) {
    Material next = m;
    next.back_white_pawns--;
    next.other_white_pawns++;
    result.push_back(next);
  }

  return result;
}

// Get all materials reachable by promotion (other pawn -> queen)
std::vector<Material> promotion_targets(const Material& m) {
  std::vector<Material> result;

  if (m.other_white_pawns > 0) {
    Material next = m;
    next.other_white_pawns--;
    next.white_queens++;
    result.push_back(next);
  }

  return result;
}

// ============================================================================
// Position-Level Parallel Tablebase Generation
// ============================================================================
//
// Uses OpenMP to parallelize the loop over positions within a single material.
// This avoids dependency bottlenecks where large materials block other work.
//
// Algorithm: Level-synchronized iteration
//   1. Initialize: mark terminals (no moves = LOSS), immediate wins
//   2. Iterate: for each UNKNOWN, check successors. WIN if any LOSS, LOSS if all WIN.
//   3. Repeat until no changes
//   4. Remaining UNKNOWNs are DRAWs

// Compute value for a single position given current table states
// Returns UNKNOWN if not yet determinable
Value compute_position_value(
    std::size_t idx,
    const Material& m,
    const std::vector<Value>& table_m,      // Current material's table
    const std::vector<Value>& table_fm,     // flip(m)'s table (empty if symmetric)
    const std::unordered_map<Material, std::vector<Value>>& deps) {

  Board board = index_to_board(idx, m);
  std::vector<Move> moves;
  generateMoves(board, moves);

  if (moves.empty()) return Value::LOSS;

  Material fm = flip(m);
  bool symmetric = (m == fm);

  bool found_loss = false;      // Found a successor that's LOSS (we can WIN)
  bool has_draw = false;        // Found a successor that's DRAW
  bool has_unknown = false;     // Found a successor that's UNKNOWN
  std::size_t num_wins = 0;     // Count of successors that are WIN

  for (const Move& move : moves) {
    Board next = makeMove(board, move);
    Material next_m = get_material(next);

    Value succ_val = Value::UNKNOWN;

    // Terminal capture (opponent eliminated)
    if (next_m.white_pieces() == 0) {
      succ_val = Value::LOSS;  // Opponent loses
    } else if (next_m.black_pieces() == 0) {
      succ_val = Value::WIN;   // Shouldn't happen, but treat as bad
    } else if (next_m == fm) {
      // Quiet move: lookup in flip(m)'s table
      std::size_t next_idx = board_to_index(next, fm);
      if (symmetric) {
        succ_val = table_m[next_idx];
      } else {
        succ_val = table_fm[next_idx];
      }
    } else {
      // Material-changing move: lookup in dependency tablebase
      auto it = deps.find(next_m);
      if (it != deps.end() && !it->second.empty()) {
        std::size_t next_idx = board_to_index(next, next_m);
        if (next_idx < it->second.size()) {
          succ_val = it->second[next_idx];
        }
      }
    }

    if (succ_val == Value::LOSS) {
      found_loss = true;  // We can win by moving here
    } else if (succ_val == Value::WIN) {
      num_wins++;
    } else if (succ_val == Value::DRAW) {
      has_draw = true;
    } else {
      has_unknown = true;
    }
  }

  // Determine our value
  if (found_loss) {
    return Value::WIN;
  }
  if (num_wins == moves.size()) {
    return Value::LOSS;  // All successors are wins for opponent
  }
  if (!has_unknown && has_draw) {
    return Value::DRAW;  // No unknowns left, we have a draw escape
  }
  if (!has_unknown) {
    return Value::DRAW;  // No unknowns, no losses found, not all wins = draw
  }

  return Value::UNKNOWN;  // Still undetermined
}

// Compute value for a single position using compressed tablebases for dependencies
// Uses full vectors for same-material lookups (m, flip(m))
// Uses CompressedTablebaseManager for different-material lookups
Value compute_position_value_compressed(
    std::size_t idx,
    const Material& m,
    const std::vector<Value>& table_m,
    const std::vector<Value>& table_fm,
    CompressedTablebaseManager* manager) {

  Board board = index_to_board(idx, m);
  std::vector<Move> moves;
  generateMoves(board, moves);

  if (moves.empty()) return Value::LOSS;

  Material fm = flip(m);
  bool symmetric = (m == fm);

  bool found_loss = false;
  bool has_draw = false;
  bool has_unknown = false;
  std::size_t num_wins = 0;

  for (const Move& move : moves) {
    Board next = makeMove(board, move);
    Material next_m = get_material(next);

    Value succ_val = Value::UNKNOWN;

    // Terminal capture (opponent eliminated)
    if (next_m.white_pieces() == 0) {
      succ_val = Value::LOSS;  // Opponent loses
    } else if (next_m.black_pieces() == 0) {
      succ_val = Value::WIN;   // Shouldn't happen, but treat as bad
    } else if (next_m == fm) {
      // Quiet move: lookup in flip(m)'s table
      std::size_t next_idx = board_to_index(next, fm);
      if (symmetric) {
        succ_val = table_m[next_idx];
      } else {
        succ_val = table_fm[next_idx];
      }
    } else {
      // Material-changing move: lookup in compressed tablebase via manager
      succ_val = manager->lookup_wdl(next);
    }

    if (succ_val == Value::LOSS) {
      found_loss = true;
    } else if (succ_val == Value::WIN) {
      num_wins++;
    } else if (succ_val == Value::DRAW) {
      has_draw = true;
    } else {
      has_unknown = true;
    }
  }

  if (found_loss) {
    return Value::WIN;
  }
  if (num_wins == moves.size()) {
    return Value::LOSS;
  }
  if (!has_unknown && has_draw) {
    return Value::DRAW;
  }
  if (!has_unknown) {
    return Value::DRAW;
  }

  return Value::UNKNOWN;
}

// Generate tablebase(s) for material M (and flip(M) if different)
// Uses position-level parallelism with level-synchronized iteration
void generate_wdl_parallel(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& deps) {

  Material fm = flip(m);
  bool symmetric = (m == fm);

  std::size_t size_m = material_size(m);
  std::size_t size_fm = symmetric ? 0 : material_size(fm);

  std::cout << "  [1/5] Allocating tables: " << m << " (" << size_m << " pos)";
  if (!symmetric) std::cout << " + " << fm << " (" << size_fm << " pos)";
  std::cout << std::endl;
  std::cout.flush();

  std::vector<Value> table_m(size_m, Value::UNKNOWN);
  std::vector<Value> table_fm(size_fm, Value::UNKNOWN);

  std::cout << "  [2/5] Starting iterative solver..." << std::endl;
  std::cout.flush();

  auto start = std::chrono::high_resolution_clock::now();

  // Iterate until stable
  int iteration = 0;
  bool changed = true;

  while (changed) {
    changed = false;
    iteration++;

    if (iteration <= 5 || iteration % 10 == 0) {
      std::cout << "    Iteration " << iteration << " (" << size_m << " pos)..." << std::flush;
    }

    // Update table_m in parallel with progress tracking
    std::atomic<std::size_t> progress_m{0};
    auto last_report_m = std::chrono::steady_clock::now();

    #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
    for (std::size_t idx = 0; idx < size_m; ++idx) {
      if (table_m[idx] != Value::UNKNOWN) continue;

      Value new_val = compute_position_value(idx, m, table_m, table_fm, deps);
      if (new_val != Value::UNKNOWN) {
        table_m[idx] = new_val;
        changed = true;
      }

      // Progress reporting (only thread 0, every 5 seconds)
      std::size_t p = progress_m.fetch_add(1, std::memory_order_relaxed);
      if (omp_get_thread_num() == 0 && (p % 1000000 == 0)) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_m).count();
        if (elapsed >= 5) {
          double pct = 100.0 * p / size_m;
          std::cout << "\n      " << m << ": " << p << "/" << size_m
                    << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
          last_report_m = now;
        }
      }
    }

    if (!symmetric) {
      // Update table_fm in parallel with progress tracking
      std::atomic<std::size_t> progress_fm{0};
      auto last_report_fm = std::chrono::steady_clock::now();

      #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
      for (std::size_t idx = 0; idx < size_fm; ++idx) {
        if (table_fm[idx] != Value::UNKNOWN) continue;

        Value new_val = compute_position_value(idx, fm, table_fm, table_m, deps);
        if (new_val != Value::UNKNOWN) {
          table_fm[idx] = new_val;
          changed = true;
        }

        // Progress reporting (only thread 0, every 5 seconds)
        std::size_t p = progress_fm.fetch_add(1, std::memory_order_relaxed);
        if (omp_get_thread_num() == 0 && (p % 1000000 == 0)) {
          auto now = std::chrono::steady_clock::now();
          auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_fm).count();
          if (elapsed >= 5) {
            double pct = 100.0 * p / size_fm;
            std::cout << "\n      " << fm << ": " << p << "/" << size_fm
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
            last_report_fm = now;
          }
        }
      }
    }

    if (iteration <= 5 || iteration % 10 == 0) {
      std::cout << " " << (changed ? "changed" : "stable") << std::endl;
    }
  }

  std::cout << "  [3/5] Marking remaining positions as DRAW..." << std::endl;
  std::cout.flush();

  // Mark remaining as DRAW
  std::size_t wins_m = 0, losses_m = 0, draws_m = 0;
  #pragma omp parallel for reduction(+:wins_m,losses_m,draws_m)
  for (std::size_t idx = 0; idx < size_m; ++idx) {
    if (table_m[idx] == Value::UNKNOWN) table_m[idx] = Value::DRAW;
    if (table_m[idx] == Value::WIN) wins_m++;
    else if (table_m[idx] == Value::LOSS) losses_m++;
    else draws_m++;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  std::cout << "  [4/5] " << m << " (" << size_m << " pos): "
            << wins_m << "W/" << losses_m << "L/" << draws_m << "D, "
            << iteration << " iters, " << duration.count() << "ms" << std::endl;

  std::cout << "  [5/5] Saving tablebase to disk..." << std::endl;
  std::cout.flush();

  save_tablebase(table_m, m);
  store_wdl_tablebase(m, std::move(table_m));

  if (!symmetric) {
    std::size_t wins_fm = 0, losses_fm = 0, draws_fm = 0;
    #pragma omp parallel for reduction(+:wins_fm,losses_fm,draws_fm)
    for (std::size_t idx = 0; idx < size_fm; ++idx) {
      if (table_fm[idx] == Value::UNKNOWN) table_fm[idx] = Value::DRAW;
      if (table_fm[idx] == Value::WIN) wins_fm++;
      else if (table_fm[idx] == Value::LOSS) losses_fm++;
      else draws_fm++;
    }

    std::cout << fm << " (" << size_fm << " pos): "
              << wins_fm << "W/" << losses_fm << "L/" << draws_fm << "D" << std::endl;

    save_tablebase(table_fm, fm);
    store_wdl_tablebase(fm, std::move(table_fm));
  }
}

// Generate tablebase(s) for material M using compressed tablebases for dependencies.
// Uses position-level parallelism with thread-local managers for thread safety.
// Saves only compressed output (cwdl_*.bin), not uncompressed tb_*.bin.
void generate_wdl_parallel_compressed(
    const Material& m,
    const std::string& tb_directory) {

  Material fm = flip(m);
  bool symmetric = (m == fm);

  std::size_t size_m = material_size(m);
  std::size_t size_fm = symmetric ? 0 : material_size(fm);

  std::cout << "  [1/6] Allocating tables: " << m << " (" << size_m << " pos)";
  if (!symmetric) std::cout << " + " << fm << " (" << size_fm << " pos)";
  std::cout << std::endl;
  std::cout.flush();

  std::vector<Value> table_m(size_m, Value::UNKNOWN);
  std::vector<Value> table_fm(size_fm, Value::UNKNOWN);

  auto start = std::chrono::high_resolution_clock::now();

  // Create thread-local managers for parallel lookups
  int num_threads = omp_get_max_threads();
  std::cout << "  [2/6] Creating " << num_threads << " thread-local tablebase managers..." << std::endl;
  std::cout.flush();

  std::vector<std::unique_ptr<CompressedTablebaseManager>> thread_managers;
  for (int i = 0; i < num_threads; ++i) {
    thread_managers.push_back(
        std::make_unique<CompressedTablebaseManager>(tb_directory));
  }

  std::cout << "  [3/6] Starting iterative solver..." << std::endl;
  std::cout.flush();

  // Iterate until stable
  int iteration = 0;
  bool changed = true;

  while (changed) {
    changed = false;
    iteration++;

    if (iteration <= 5 || iteration % 10 == 0) {
      std::cout << "    Iteration " << iteration << " (" << size_m << " pos)..." << std::flush;
    }

    // Update table_m in parallel with progress tracking
    std::atomic<std::size_t> progress_m{0};

    #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
    for (std::size_t idx = 0; idx < size_m; ++idx) {
      if (table_m[idx] != Value::UNKNOWN) continue;

      auto* mgr = thread_managers[omp_get_thread_num()].get();
      Value new_val = compute_position_value_compressed(idx, m, table_m, table_fm, mgr);
      if (new_val != Value::UNKNOWN) {
        table_m[idx] = new_val;
        changed = true;
      }

      // Progress reporting (only thread 0, every 100k positions)
      std::size_t p = progress_m.fetch_add(1, std::memory_order_relaxed);
      if (omp_get_thread_num() == 0 && (p % 100000 == 0) && p > 0) {
        double pct = 100.0 * p / size_m;
        std::cout << "\r      " << m << ": " << p << "/" << size_m
                  << " (" << std::fixed << std::setprecision(1) << pct << "%)   " << std::flush;
      }
    }
    std::cout << "\r      " << m << ": " << size_m << "/" << size_m << " (100.0%)   " << std::endl;

    if (!symmetric) {
      // Update table_fm in parallel with progress tracking
      std::atomic<std::size_t> progress_fm{0};

      #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
      for (std::size_t idx = 0; idx < size_fm; ++idx) {
        if (table_fm[idx] != Value::UNKNOWN) continue;

        auto* mgr = thread_managers[omp_get_thread_num()].get();
        Value new_val = compute_position_value_compressed(idx, fm, table_fm, table_m, mgr);
        if (new_val != Value::UNKNOWN) {
          table_fm[idx] = new_val;
          changed = true;
        }

        // Progress reporting (only thread 0, every 100k positions)
        std::size_t p = progress_fm.fetch_add(1, std::memory_order_relaxed);
        if (omp_get_thread_num() == 0 && (p % 100000 == 0) && p > 0) {
          double pct = 100.0 * p / size_fm;
          std::cout << "\r      " << fm << ": " << p << "/" << size_fm
                    << " (" << std::fixed << std::setprecision(1) << pct << "%)   " << std::flush;
        }
      }
      std::cout << "\r      " << fm << ": " << size_fm << "/" << size_fm << " (100.0%)   " << std::endl;
    }

    if (iteration <= 5 || iteration % 10 == 0) {
      std::cout << " " << (changed ? "changed" : "stable") << std::endl;
    }
  }

  std::cout << "  [4/6] Marking remaining positions as DRAW..." << std::endl;
  std::cout.flush();

  // Mark remaining as DRAW
  std::size_t wins_m = 0, losses_m = 0, draws_m = 0;
  #pragma omp parallel for reduction(+:wins_m,losses_m,draws_m)
  for (std::size_t idx = 0; idx < size_m; ++idx) {
    if (table_m[idx] == Value::UNKNOWN) table_m[idx] = Value::DRAW;
    if (table_m[idx] == Value::WIN) wins_m++;
    else if (table_m[idx] == Value::LOSS) losses_m++;
    else draws_m++;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  std::cout << "  [5/9] " << m << " (" << size_m << " pos): "
            << wins_m << "W/" << losses_m << "L/" << draws_m << "D, "
            << iteration << " iters, " << duration.count() << "ms" << std::endl;

  // Also mark table_fm remaining as DRAW before verification
  if (!symmetric) {
    #pragma omp parallel for
    for (std::size_t idx = 0; idx < size_fm; ++idx) {
      if (table_fm[idx] == Value::UNKNOWN) table_fm[idx] = Value::DRAW;
    }
  }

  // === VERIFICATION STEP 1: Verify generated values ===
  std::cout << "  [6/9] Verifying generated tablebase..." << std::endl;
  std::cout.flush();

  if (!verify_generated_tablebase(table_m, m, table_fm, thread_managers[0].get())) {
    std::cerr << "FATAL: Generation verification failed for " << m << "\n";
    std::exit(1);
  }
  std::cout << "    " << m << ": OK" << std::endl;

  if (!symmetric) {
    if (!verify_generated_tablebase(table_fm, fm, table_m, thread_managers[0].get())) {
      std::cerr << "FATAL: Generation verification failed for " << fm << "\n";
      std::exit(1);
    }
    std::cout << "    " << fm << ": OK" << std::endl;
  }

  // === VERIFICATION STEP 2: Verify compression roundtrip ===
  std::cout << "  [7/9] Verifying compression roundtrip..." << std::endl;
  std::cout.flush();

  if (!verify_compression_roundtrip(table_m, m)) {
    std::cerr << "FATAL: Compression roundtrip failed for " << m << "\n";
    std::exit(1);
  }
  std::cout << "    " << m << ": OK" << std::endl;

  if (!symmetric) {
    if (!verify_compression_roundtrip(table_fm, fm)) {
      std::cerr << "FATAL: Compression roundtrip failed for " << fm << "\n";
      std::exit(1);
    }
    std::cout << "    " << fm << ": OK" << std::endl;
  }

  // === COMPRESSION AND SAVE ===
  std::cout << "  [8/9] Compressing and saving..." << std::endl;
  std::cout.flush();

  // Mark don't-care positions and compress
  CompressionStats cstats;
  std::vector<Value> marked_m = mark_dont_care_positions(table_m, m, cstats);
  CompressedTablebase ctb_m = compress_tablebase(marked_m, m);

  std::string filename_m = tb_directory + "/" + compressed_tablebase_filename(m);
  save_compressed_tablebase(ctb_m, filename_m);

  BlockCompressionStats bstats_m = analyze_block_compression(ctb_m);
  std::cout << "    Saved " << filename_m << " ("
            << bstats_m.compressed_size << " bytes, "
            << std::fixed << std::setprecision(1) << bstats_m.compression_ratio() << "x ratio)"
            << std::endl;

  // === VERIFICATION STEP 3: Verify compressed lookup ===
  std::cout << "  [9/9] Verifying compressed lookup..." << std::endl;
  std::cout.flush();

  if (!verify_compressed_lookup(table_m, ctb_m, m)) {
    std::cerr << "FATAL: Compressed lookup verification failed for " << m << "\n";
    std::exit(1);
  }
  std::cout << "    " << m << ": OK" << std::endl;

  if (!symmetric) {
    std::size_t wins_fm = 0, losses_fm = 0, draws_fm = 0;
    for (std::size_t idx = 0; idx < size_fm; ++idx) {
      if (table_fm[idx] == Value::WIN) wins_fm++;
      else if (table_fm[idx] == Value::LOSS) losses_fm++;
      else draws_fm++;
    }

    std::cout << "    " << fm << " (" << size_fm << " pos): "
              << wins_fm << "W/" << losses_fm << "L/" << draws_fm << "D" << std::endl;

    CompressionStats cstats_fm;
    std::vector<Value> marked_fm = mark_dont_care_positions(table_fm, fm, cstats_fm);
    CompressedTablebase ctb_fm = compress_tablebase(marked_fm, fm);

    std::string filename_fm = tb_directory + "/" + compressed_tablebase_filename(fm);
    save_compressed_tablebase(ctb_fm, filename_fm);

    BlockCompressionStats bstats_fm = analyze_block_compression(ctb_fm);
    std::cout << "    Saved " << filename_fm << " ("
              << bstats_fm.compressed_size << " bytes, "
              << std::fixed << std::setprecision(1) << bstats_fm.compression_ratio() << "x ratio)"
              << std::endl;

    // Verify compressed lookup for flip material
    if (!verify_compressed_lookup(table_fm, ctb_fm, fm)) {
      std::cerr << "FATAL: Compressed lookup verification failed for " << fm << "\n";
      std::exit(1);
    }
    std::cout << "    " << fm << ": OK" << std::endl;
  }

  std::cout << "  All verifications passed.\n" << std::endl;
}

// Legacy single-threaded generate for compatibility
// (Used by solve_pair which needs iterative refinement)
std::vector<Value> generate_tablebase(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& sub_tablebases,
    Stats& stats) {

  std::size_t size = material_size(m);
  stats.total_positions = size;
  Material fm = flip(m);

  std::vector<Value> table(size, Value::UNKNOWN);

  // Get flip(m)'s table if available (for non-symmetric materials)
  std::vector<Value> table_fm;
  if (!(m == fm)) {
    auto it = sub_tablebases.find(fm);
    if (it != sub_tablebases.end()) {
      table_fm = it->second;
    }
  }

  // Iterate until stable
  bool changed = true;
  while (changed) {
    changed = false;

    for (std::size_t idx = 0; idx < size; ++idx) {
      if (table[idx] != Value::UNKNOWN) continue;

      Value new_val = compute_position_value(idx, m, table, table_fm, sub_tablebases);
      if (new_val != Value::UNKNOWN) {
        table[idx] = new_val;
        changed = true;
      }
    }
  }

  // Count and mark remaining as DRAW
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (table[idx] == Value::UNKNOWN) {
      table[idx] = Value::DRAW;
    }
    if (table[idx] == Value::WIN) stats.wins++;
    else if (table[idx] == Value::LOSS) stats.losses++;
    else stats.draws++;
  }

  return table;
}

// ============================================================================
// Position-Level Parallel DTM Generation
// ============================================================================

// Compute DTM value for a single position given current table states
// Returns DTM_UNKNOWN if not yet determinable
DTM compute_dtm_value(
    std::size_t idx,
    const Material& m,
    Value wdl_val,
    const std::vector<DTM>& dtm_m,
    const std::vector<DTM>& dtm_fm,
    const std::unordered_map<Material, std::vector<DTM>>& dtm_deps) {

  // DRAW positions are always DTM_DRAW
  if (wdl_val == Value::DRAW) return DTM_DRAW;

  Board board = index_to_board(idx, m);
  std::vector<Move> moves;
  generateMoves(board, moves);

  // Terminal LOSS (no moves)
  if (moves.empty()) return DTM_LOSS_TERMINAL;

  Material fm = flip(m);
  bool symmetric = (m == fm);

  if (wdl_val == Value::WIN) {
    // Find opponent's quickest LOSS (highest negative DTM = fewest moves)
    // Process all successors, take the best among those with known DTM
    DTM best = DTM_UNKNOWN;

    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      Material next_m = get_material(next);
      DTM succ_dtm = DTM_UNKNOWN;

      // Terminal capture
      if (next_m.white_pieces() == 0) {
        succ_dtm = DTM_LOSS_TERMINAL;
      } else if (next_m.black_pieces() == 0) {
        continue;  // Shouldn't happen for WIN position
      } else if (next_m == fm) {
        // Same material after flip
        std::size_t next_idx = board_to_index(next, fm);
        if (symmetric) {
          succ_dtm = dtm_m[next_idx];
        } else {
          succ_dtm = dtm_fm[next_idx];
        }
      } else {
        // Different material (capture/promotion)
        auto it = dtm_deps.find(next_m);
        if (it != dtm_deps.end()) {
          std::size_t next_idx = board_to_index(next, next_m);
          if (next_idx < it->second.size()) {
            succ_dtm = it->second[next_idx];
          }
        }
      }

      // Only consider LOSS successors (negative DTM, excluding UNKNOWN)
      if (succ_dtm < DTM_DRAW && succ_dtm != DTM_UNKNOWN) {
        // Track quickest loss for opponent (highest negative = fastest win for us)
        if (best == DTM_UNKNOWN) {
          best = succ_dtm;
        } else if (succ_dtm == DTM_LOSS_TERMINAL) {
          best = DTM_LOSS_TERMINAL;
        } else if (best != DTM_LOSS_TERMINAL && succ_dtm > best) {
          best = succ_dtm;
        }
      }
    }

    if (best != DTM_UNKNOWN) {
      int opp_moves = (best == DTM_LOSS_TERMINAL) ? 0 : -best;
      return dtm_win(opp_moves + 1);
    }
    return DTM_UNKNOWN;
  }

  // LOSS: find opponent's slowest WIN (highest positive DTM)
  DTM worst = DTM_UNKNOWN;
  bool all_known = true;

  for (const Move& move : moves) {
    Board next = makeMove(board, move);
    Material next_m = get_material(next);
    DTM succ_dtm = DTM_UNKNOWN;

    if (next_m.white_pieces() == 0) {
      succ_dtm = DTM_LOSS_TERMINAL;  // Terminal loss for opponent
    } else if (next_m.black_pieces() == 0) {
      succ_dtm = dtm_win(1);  // We captured everything
    } else if (next_m == fm) {
      std::size_t next_idx = board_to_index(next, fm);
      if (symmetric) {
        succ_dtm = dtm_m[next_idx];
      } else {
        succ_dtm = dtm_fm[next_idx];
      }
    } else {
      auto it = dtm_deps.find(next_m);
      if (it != dtm_deps.end()) {
        std::size_t next_idx = board_to_index(next, next_m);
        if (next_idx < it->second.size()) {
          succ_dtm = it->second[next_idx];
        }
      }
    }

    if (succ_dtm == DTM_UNKNOWN) {
      all_known = false;
      break;
    }

    // Track slowest win for opponent (we want to survive longest)
    if (succ_dtm > DTM_DRAW) {
      if (worst == DTM_UNKNOWN || succ_dtm > worst) {
        worst = succ_dtm;
      }
    }
  }

  if (all_known && worst != DTM_UNKNOWN) {
    return dtm_loss(worst);
  }
  return DTM_UNKNOWN;
}

// Generate DTM tablebase(s) for material M (and flip(M) if different)
// Uses position-level parallelism with level-synchronized iteration
void generate_dtm_parallel(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& wdl_deps,
    const std::unordered_map<Material, std::vector<DTM>>& dtm_deps) {

  Material fm = flip(m);
  bool symmetric = (m == fm);

  std::size_t size_m = material_size(m);
  std::size_t size_fm = symmetric ? 0 : material_size(fm);

  // Load WDL tables
  auto wdl_m_it = wdl_deps.find(m);
  auto wdl_fm_it = wdl_deps.find(fm);
  if (wdl_m_it == wdl_deps.end() || wdl_m_it->second.empty()) {
    std::cerr << "Error: WDL not found for " << m << std::endl;
    return;
  }
  const std::vector<Value>& wdl_m = wdl_m_it->second;
  const std::vector<Value>& wdl_fm = symmetric ? wdl_m :
    (wdl_fm_it != wdl_deps.end() ? wdl_fm_it->second : wdl_m);

  std::vector<DTM> dtm_m(size_m, DTM_UNKNOWN);
  std::vector<DTM> dtm_fm(size_fm, DTM_UNKNOWN);

  auto start = std::chrono::high_resolution_clock::now();

  // Initialize: DRAW = 0, terminal LOSS = -128
  #pragma omp parallel for schedule(dynamic, 1024)
  for (std::size_t idx = 0; idx < size_m; ++idx) {
    if (wdl_m[idx] == Value::DRAW) {
      dtm_m[idx] = DTM_DRAW;
    } else if (wdl_m[idx] == Value::LOSS) {
      Board board = index_to_board(idx, m);
      std::vector<Move> moves;
      generateMoves(board, moves);
      if (moves.empty()) dtm_m[idx] = DTM_LOSS_TERMINAL;
    }
  }

  if (!symmetric) {
    #pragma omp parallel for schedule(dynamic, 1024)
    for (std::size_t idx = 0; idx < size_fm; ++idx) {
      if (wdl_fm[idx] == Value::DRAW) {
        dtm_fm[idx] = DTM_DRAW;
      } else if (wdl_fm[idx] == Value::LOSS) {
        Board board = index_to_board(idx, fm);
        std::vector<Move> moves;
        generateMoves(board, moves);
        if (moves.empty()) dtm_fm[idx] = DTM_LOSS_TERMINAL;
      }
    }
  }

  // Iterate until stable
  int iteration = 0;
  bool changed = true;
  constexpr int MAX_ITERATIONS = 200;

  while (changed && iteration < MAX_ITERATIONS) {
    changed = false;
    iteration++;

    // Update dtm_m in parallel
    #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
    for (std::size_t idx = 0; idx < size_m; ++idx) {
      if (wdl_m[idx] == Value::DRAW) continue;

      DTM new_val = compute_dtm_value(idx, m, wdl_m[idx], dtm_m, dtm_fm, dtm_deps);
      if (new_val != DTM_UNKNOWN && dtm_m[idx] != new_val) {
        // For WIN: update if better (smaller = faster win)
        // For LOSS: update if different (as successor WINs stabilize)
        if (wdl_m[idx] == Value::WIN) {
          if (dtm_m[idx] == DTM_UNKNOWN || new_val < dtm_m[idx]) {
            dtm_m[idx] = new_val;
            changed = true;
          }
        } else {
          // LOSS: always update when different (value improves as WINs stabilize)
          dtm_m[idx] = new_val;
          changed = true;
        }
      }
    }

    if (!symmetric) {
      // Update dtm_fm in parallel
      #pragma omp parallel for schedule(dynamic, 1024) reduction(||:changed)
      for (std::size_t idx = 0; idx < size_fm; ++idx) {
        if (wdl_fm[idx] == Value::DRAW) continue;

        DTM new_val = compute_dtm_value(idx, fm, wdl_fm[idx], dtm_fm, dtm_m, dtm_deps);
        if (new_val != DTM_UNKNOWN && dtm_fm[idx] != new_val) {
          if (wdl_fm[idx] == Value::WIN) {
            if (dtm_fm[idx] == DTM_UNKNOWN || new_val < dtm_fm[idx]) {
              dtm_fm[idx] = new_val;
              changed = true;
            }
          } else {
            dtm_fm[idx] = new_val;
            changed = true;
          }
        }
      }
    }
  }

  if (iteration >= MAX_ITERATIONS) {
    std::cerr << "WARNING: DTM hit max iterations for " << m << std::endl;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Statistics helper
  auto count_stats = [](const std::vector<DTM>& dtm) {
    int max_win = 0, max_loss = 0;
    std::size_t w = 0, l = 0, d = 0;
    for (DTM v : dtm) {
      if (v == DTM_DRAW) d++;
      else if (v > 0) { w++; int p = dtm_to_plies(v); if (p > max_win) max_win = p; }
      else if (v != DTM_UNKNOWN) { l++; int p = dtm_to_plies(v); if (p > max_loss) max_loss = p; }
    }
    return std::make_tuple(w, l, d, max_win, max_loss);
  };

  auto [w_m, l_m, d_m, mw_m, ml_m] = count_stats(dtm_m);
  std::cout << "DTM " << m << " (" << size_m << " pos): "
            << w_m << "W/" << l_m << "L/" << d_m << "D"
            << " (max " << mw_m << "/" << ml_m << " plies), "
            << iteration << " iters, " << duration.count() << "ms" << std::endl;

  save_dtm(dtm_m, m);
  store_dtm_tablebase(m, std::move(dtm_m));

  if (!symmetric) {
    auto [w_fm, l_fm, d_fm, mw_fm, ml_fm] = count_stats(dtm_fm);
    std::cout << "DTM " << fm << " (" << size_fm << " pos): "
              << w_fm << "W/" << l_fm << "L/" << d_fm << "D"
              << " (max " << mw_fm << "/" << ml_fm << " plies)" << std::endl;

    save_dtm(dtm_fm, fm);
    store_dtm_tablebase(fm, std::move(dtm_fm));
  }
}

// Get direct dependencies for scheduling (not transitive closure)
// Only includes materials reachable by a single capture, advancement, or promotion
// Excludes flip(m) since that's handled specially by solve_pair
std::vector<Material> get_dependencies(const Material& m) {
  std::unordered_set<Material> deps;
  Material flipped_m = flip(m);

  // Helper to add a dependency (and its flip) if valid
  auto add_dep = [&](const Material& target) {
    if (!(target == m) && !(target == flipped_m)) {
      deps.insert(target);
      Material ft = flip(target);
      if (!(ft == m) && !(ft == flipped_m)) {
        deps.insert(ft);
      }
    }
  };

  // Process both m and flip(m) to get all direct transitions
  for (const Material& curr : {m, flipped_m}) {
    // Capture targets (removes one opponent piece)
    for (const Material& target : capture_targets(curr)) {
      add_dep(target);
    }

    // Advancement targets (back pawn -> normal pawn)
    for (const Material& target : advancement_targets(curr)) {
      add_dep(target);
    }

    // Promotion targets (normal pawn -> queen)
    for (const Material& target : promotion_targets(curr)) {
      add_dep(target);
    }

    // Also consider opponent's advancement and promotion (after board flip)
    Material curr_flipped = flip(curr);
    for (const Material& target : advancement_targets(curr_flipped)) {
      add_dep(flip(target));
    }
    for (const Material& target : promotion_targets(curr_flipped)) {
      add_dep(flip(target));
    }
  }

  return std::vector<Material>(deps.begin(), deps.end());
}

// Get ALL sub-materials needed for generating tablebase (transitive closure)
// A single move can combine captures with advancement/promotion, so we need the
// full transitive closure of all material-changing operations
std::vector<Material> get_sub_materials(const Material& m) {
  std::unordered_set<Material> deps;
  Material flipped_m = flip(m);
  std::vector<Material> to_process = {m, flipped_m};

  while (!to_process.empty()) {
    Material curr = to_process.back();
    to_process.pop_back();

    // Helper to add a dependency and queue for processing
    auto add_and_queue = [&](const Material& target) {
      if (deps.find(target) == deps.end() && !(target == m) && !(target == flipped_m)) {
        deps.insert(target);
        to_process.push_back(target);
      }
      Material flipped = flip(target);
      if (deps.find(flipped) == deps.end() && !(flipped == m) && !(flipped == flipped_m)) {
        deps.insert(flipped);
        to_process.push_back(flipped);
      }
    };

    // Capture targets (transitive - follow all capture chains)
    for (const Material& target : capture_targets(curr)) {
      add_and_queue(target);
    }

    // Advancement targets (transitive - can chain with captures)
    for (const Material& target : advancement_targets(curr)) {
      add_and_queue(target);
    }
    Material curr_flipped = flip(curr);
    for (const Material& target : advancement_targets(curr_flipped)) {
      add_and_queue(flip(target));
    }

    // Promotion targets (transitive - can chain with captures)
    for (const Material& target : promotion_targets(curr)) {
      add_and_queue(target);
    }
    for (const Material& target : promotion_targets(curr_flipped)) {
      add_and_queue(flip(target));
    }
  }

  return std::vector<Material>(deps.begin(), deps.end());
}

// Solve a single material (for symmetric or terminal materials)
void solve_single(const Material& m,
                  std::unordered_map<Material, std::vector<Value>>& tablebases) {

  std::cout << "Solving material " << m << " (" << material_size(m) << " positions)" << std::endl;

  Stats stats;
  auto start = std::chrono::high_resolution_clock::now();

  std::vector<Value> table = generate_tablebase(m, tablebases, stats);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  std::cout << "  Completed in " << duration.count() << "ms" << std::endl;
  std::cout << "  Wins: " << stats.wins << " (" << (100.0 * stats.wins / stats.total_positions) << "%)" << std::endl;
  std::cout << "  Losses: " << stats.losses << " (" << (100.0 * stats.losses / stats.total_positions) << "%)" << std::endl;
  std::cout << "  Draws: " << stats.draws << " (" << (100.0 * stats.draws / stats.total_positions) << "%)" << std::endl;
  std::cout << "  With captures: " << stats.with_captures << std::endl;

  save_tablebase(table, m);
  std::cout << "  Saved to " << tablebase_filename(m) << std::endl;

  tablebases[m] = std::move(table);
}

// Solve a pair of mutually-dependent materials iteratively
void solve_pair(const Material& m1, const Material& m2,
                std::unordered_map<Material, std::vector<Value>>& tablebases) {

  std::cout << "Solving material pair " << m1 << " <-> " << m2 << std::endl;

  std::size_t size1 = material_size(m1);
  std::size_t size2 = material_size(m2);

  // Initialize both tables as UNKNOWN (represented as DRAW for now)
  std::vector<Value> table1(size1, Value::DRAW);
  std::vector<Value> table2(size2, Value::DRAW);

  // Add to tablebases so generate_tablebase can look them up
  tablebases[m1] = table1;
  tablebases[m2] = table2;

  // Iterate until stable
  bool changed = true;
  int iteration = 0;
  while (changed && iteration < 100) {
    changed = false;
    iteration++;

    // Generate m1 using current m2
    Stats stats1;
    std::vector<Value> new_table1 = generate_tablebase(m1, tablebases, stats1);
    for (std::size_t i = 0; i < size1; ++i) {
      if (table1[i] != new_table1[i]) {
        changed = true;
        table1[i] = new_table1[i];
      }
    }
    tablebases[m1] = table1;

    // Generate m2 using current m1
    Stats stats2;
    std::vector<Value> new_table2 = generate_tablebase(m2, tablebases, stats2);
    for (std::size_t i = 0; i < size2; ++i) {
      if (table2[i] != new_table2[i]) {
        changed = true;
        table2[i] = new_table2[i];
      }
    }
    tablebases[m2] = table2;

    std::cout << "  Iteration " << iteration << ": " << (changed ? "changed" : "stable") << std::endl;
  }

  // Count final statistics
  std::size_t wins1 = 0, losses1 = 0, draws1 = 0;
  for (Value v : table1) {
    if (v == Value::WIN) wins1++;
    else if (v == Value::LOSS) losses1++;
    else draws1++;
  }
  std::cout << "  " << m1 << ": " << wins1 << " wins, " << losses1 << " losses, " << draws1 << " draws" << std::endl;

  std::size_t wins2 = 0, losses2 = 0, draws2 = 0;
  for (Value v : table2) {
    if (v == Value::WIN) wins2++;
    else if (v == Value::LOSS) losses2++;
    else draws2++;
  }
  std::cout << "  " << m2 << ": " << wins2 << " wins, " << losses2 << " losses, " << draws2 << " draws" << std::endl;

  save_tablebase(table1, m1);
  save_tablebase(table2, m2);
  std::cout << "  Saved to " << tablebase_filename(m1) << " and " << tablebase_filename(m2) << std::endl;
}

// ============================================================================
// Thread-Safe Parallel Solvers (for OpenMP parallelization)
// ============================================================================

// Verify all dependencies are loaded, abort if any are missing
void verify_dependencies(const Material& m,
                         const std::vector<Material>& deps,
                         const std::unordered_map<Material, std::vector<Value>>& loaded) {
  std::vector<Material> missing;
  for (const Material& dep : deps) {
    // Skip terminal materials (one side has 0 pieces) - no tablebase needed
    if (dep.white_pieces() == 0 || dep.black_pieces() == 0) continue;
    if (loaded.find(dep) == loaded.end()) {
      missing.push_back(dep);
    }
  }
  if (!missing.empty()) {
    std::cerr << "\nFATAL: Missing dependencies for " << m << ":\n";
    for (const Material& dep : missing) {
      std::cerr << "  " << dep << " (not generated or not on disk)\n";
    }
    std::cerr << "This indicates a bug in dependency scheduling or all_materials().\n";
    std::cerr << "Aborting to prevent generating incorrect tablebase.\n";
    std::exit(1);
  }
}

// Solve all materials up to max_pieces using position-level parallelism
// Materials are processed sequentially in dependency order, but each material
// uses all available cores for parallel position processing.
// If load_existing is true, try to load tablebases from disk instead of generating.
void solve_all_parallel(int max_pieces, bool load_existing = false) {
  std::vector<Material> all = all_materials(max_pieces);

  std::cout << "=== Parallel Tablebase Generation ===" << std::endl;
  std::cout << "Using " << omp_get_max_threads() << " threads (position-level parallelism)" << std::endl;
  if (load_existing) {
    std::cout << "Mode: Load existing tablebases from disk when available" << std::endl;
  }
  std::cout << "Total materials: " << all.size() << std::endl;

  // Canonicalize materials: for asymmetric pairs, pick one representative
  std::unordered_map<Material, Material> canonical;
  std::unordered_set<Material> canonical_set;

  auto material_less = [](const Material& a, const Material& b) {
    if (a.back_white_pawns != b.back_white_pawns) return a.back_white_pawns < b.back_white_pawns;
    if (a.back_black_pawns != b.back_black_pawns) return a.back_black_pawns < b.back_black_pawns;
    if (a.other_white_pawns != b.other_white_pawns) return a.other_white_pawns < b.other_white_pawns;
    if (a.other_black_pawns != b.other_black_pawns) return a.other_black_pawns < b.other_black_pawns;
    if (a.white_queens != b.white_queens) return a.white_queens < b.white_queens;
    return a.black_queens < b.black_queens;
  };

  for (const Material& m : all) {
    Material f = flip(m);
    if (canonical.find(m) == canonical.end()) {
      Material c = material_less(m, f) ? m : f;
      canonical[m] = c;
      canonical[f] = c;
      canonical_set.insert(c);
    }
  }

  std::vector<Material> work_units(canonical_set.begin(), canonical_set.end());
  std::cout << "Work units (canonical materials): " << work_units.size() << std::endl;

  // Build dependency graph
  std::unordered_map<Material, std::unordered_set<Material>> deps_map;
  std::unordered_map<Material, int> dep_count;

  for (const Material& m : work_units) {
    std::vector<Material> deps = get_dependencies(m);
    std::unordered_set<Material> canonical_deps;
    for (const Material& dep : deps) {
      Material c = canonical[dep];
      if (!(c == m) && canonical_set.find(c) != canonical_set.end()) {
        canonical_deps.insert(c);
      }
    }
    deps_map[m] = canonical_deps;
    dep_count[m] = canonical_deps.size();
  }

  // Topological sort using Kahn's algorithm
  std::vector<Material> sorted;
  std::queue<Material> ready;

  for (const Material& m : work_units) {
    if (dep_count[m] == 0) {
      ready.push(m);
    }
  }

  while (!ready.empty()) {
    Material m = ready.front();
    ready.pop();
    sorted.push_back(m);

    for (const Material& other : work_units) {
      if (deps_map[other].count(m)) {
        dep_count[other]--;
        if (dep_count[other] == 0) {
          ready.push(other);
        }
      }
    }
  }

  if (sorted.size() != work_units.size()) {
    std::cerr << "ERROR: Dependency cycle detected!" << std::endl;
    return;
  }

  std::cout << std::endl;

  auto total_start = std::chrono::high_resolution_clock::now();

  // Process materials in dependency order
  for (const Material& m : sorted) {
    Material f = flip(m);

    // Try to load from disk if requested
    if (load_existing) {
      std::vector<Value> tb_m = load_tablebase(m);
      if (!tb_m.empty()) {
        store_wdl_tablebase(m, std::move(tb_m));
        if (!(f == m)) {
          std::vector<Value> tb_f = load_tablebase(f);
          if (!tb_f.empty()) {
            store_wdl_tablebase(f, std::move(tb_f));
          }
        }
        std::cout << "Loaded " << m;
        if (!(f == m)) std::cout << " <-> " << f;
        std::cout << " from disk" << std::endl;
        continue;
      }
    }

    std::cout << "\n" << m;
    if (!(f == m)) std::cout << " <-> " << f;
    std::cout << " (" << m.total_pieces() << " pieces)" << std::endl;

    std::cout << "  Loading dependencies..." << std::endl;
    std::cout.flush();

    // Build dependency tablebases for this material
    std::vector<Material> deps = get_sub_materials(m);
    std::unordered_map<Material, std::vector<Value>> dep_tables;

    for (const Material& dep : deps) {
      // Skip terminal materials
      if (dep.white_pieces() == 0 || dep.black_pieces() == 0) continue;

      std::vector<Value> tb = get_wdl_tablebase(dep);
      if (tb.empty()) {
        tb = load_tablebase(dep);
      }
      if (!tb.empty()) {
        dep_tables[dep] = std::move(tb);
      }
    }

    std::cout << "  Loaded " << dep_tables.size() << " dependency tablebases" << std::endl;
    std::cout << "  Verifying dependencies..." << std::endl;
    std::cout.flush();

    // Verify dependencies
    verify_dependencies(m, deps, dep_tables);

    // Generate using position-level parallelism
    generate_wdl_parallel(m, dep_tables);
  }

  auto total_end = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end - total_start);
  std::cout << "\nTotal generation time: " << total_duration.count() << "s" << std::endl;
}

// Solve all materials up to max_pieces using compressed-only mode.
// For large endgames (>= threshold), uses compressed dependencies to save memory.
// Small endgames use the traditional method but also save compressed output.
void solve_all_parallel_compressed(int max_pieces, int threshold, const std::string& tb_directory) {
  std::vector<Material> all = all_materials(max_pieces);

  std::cout << "=== Compressed-Only Tablebase Generation ===" << std::endl;
  std::cout << "Using " << omp_get_max_threads() << " threads (position-level parallelism)" << std::endl;
  std::cout << "Compressed mode threshold: " << threshold << "+ pieces" << std::endl;
  std::cout << "Output directory: " << tb_directory << std::endl;
  std::cout << "Total materials: " << all.size() << std::endl;

  // Canonicalize materials
  std::unordered_map<Material, Material> canonical;
  std::unordered_set<Material> canonical_set;

  auto material_less = [](const Material& a, const Material& b) {
    if (a.back_white_pawns != b.back_white_pawns) return a.back_white_pawns < b.back_white_pawns;
    if (a.back_black_pawns != b.back_black_pawns) return a.back_black_pawns < b.back_black_pawns;
    if (a.other_white_pawns != b.other_white_pawns) return a.other_white_pawns < b.other_white_pawns;
    if (a.other_black_pawns != b.other_black_pawns) return a.other_black_pawns < b.other_black_pawns;
    if (a.white_queens != b.white_queens) return a.white_queens < b.white_queens;
    return a.black_queens < b.black_queens;
  };

  for (const Material& m : all) {
    Material f = flip(m);
    if (canonical.find(m) == canonical.end()) {
      Material c = material_less(m, f) ? m : f;
      canonical[m] = c;
      canonical[f] = c;
      canonical_set.insert(c);
    }
  }

  std::vector<Material> work_units(canonical_set.begin(), canonical_set.end());
  std::cout << "Work units (canonical materials): " << work_units.size() << std::endl;

  // Build dependency graph
  std::unordered_map<Material, std::unordered_set<Material>> deps_map;
  std::unordered_map<Material, int> dep_count;

  for (const Material& m : work_units) {
    std::vector<Material> deps = get_dependencies(m);
    std::unordered_set<Material> canonical_deps;
    for (const Material& dep : deps) {
      Material c = canonical[dep];
      if (!(c == m) && canonical_set.find(c) != canonical_set.end()) {
        canonical_deps.insert(c);
      }
    }
    deps_map[m] = canonical_deps;
    dep_count[m] = canonical_deps.size();
  }

  // Topological sort using Kahn's algorithm
  std::vector<Material> sorted;
  std::queue<Material> ready;

  for (const Material& m : work_units) {
    if (dep_count[m] == 0) {
      ready.push(m);
    }
  }

  while (!ready.empty()) {
    Material m = ready.front();
    ready.pop();
    sorted.push_back(m);

    for (const Material& other : work_units) {
      if (deps_map[other].count(m)) {
        dep_count[other]--;
        if (dep_count[other] == 0) {
          ready.push(other);
        }
      }
    }
  }

  if (sorted.size() != work_units.size()) {
    std::cerr << "ERROR: Dependency cycle detected!" << std::endl;
    return;
  }

  std::cout << std::endl;

  auto total_start = std::chrono::high_resolution_clock::now();

  // Create a manager to check for existing compressed tablebases
  CompressedTablebaseManager check_manager(tb_directory);

  // Process materials in dependency order
  for (const Material& m : sorted) {
    Material f = flip(m);

    // Check if already exists (compressed)
    if (check_manager.get_tablebase(m)) {
      std::cout << "Skipping " << m;
      if (!(f == m)) std::cout << " <-> " << f;
      std::cout << " (cwdl already exists)" << std::endl;
      continue;
    }

    if (m.total_pieces() >= threshold) {
      // Large endgame: use compressed mode
      std::cout << "\n[COMPRESSED] " << m;
      if (!(f == m)) std::cout << " <-> " << f;
      std::cout << " (" << m.total_pieces() << " pieces)" << std::endl;
      generate_wdl_parallel_compressed(m, tb_directory);
      // Force reload of the manager cache after new tablebase is saved
      check_manager.clear();
    } else {
      // Small endgame: use traditional method + save compressed copy
      std::cout << "\n[TRADITIONAL] " << m;
      if (!(f == m)) std::cout << " <-> " << f;
      std::cout << " (" << m.total_pieces() << " pieces)" << std::endl;

      std::cout << "  Loading dependencies..." << std::endl;
      std::cout.flush();

      std::vector<Material> deps = get_sub_materials(m);
      std::unordered_map<Material, std::vector<Value>> dep_tables;

      for (const Material& dep : deps) {
        if (dep.white_pieces() == 0 || dep.black_pieces() == 0) continue;

        std::vector<Value> tb = get_wdl_tablebase(dep);
        if (tb.empty()) {
          tb = load_tablebase(dep);
        }
        if (!tb.empty()) {
          dep_tables[dep] = std::move(tb);
        }
      }

      std::cout << "  Loaded " << dep_tables.size() << " dependency tablebases" << std::endl;
      std::cout << "  Verifying dependencies..." << std::endl;
      std::cout.flush();

      // Verify dependencies
      verify_dependencies(m, deps, dep_tables);

      // Generate using position-level parallelism
      generate_wdl_parallel(m, dep_tables);

      // Also save compressed versions
      std::vector<Value> table_m = get_wdl_tablebase(m);
      if (!table_m.empty()) {
        CompressionStats cstats_m;
        std::vector<Value> marked_m = mark_dont_care_positions(table_m, m, cstats_m);
        CompressedTablebase ctb_m = compress_tablebase(marked_m, m);
        std::string filename_m = tb_directory + "/" + compressed_tablebase_filename(m);
        save_compressed_tablebase(ctb_m, filename_m);

        BlockCompressionStats bstats_m = analyze_block_compression(ctb_m);
        std::cout << "  Saved " << filename_m << " ("
                  << bstats_m.compressed_size << " bytes, "
                  << std::fixed << std::setprecision(1) << bstats_m.compression_ratio() << "x ratio)"
                  << std::endl;
      }

      if (!(f == m)) {
        std::vector<Value> table_fm = get_wdl_tablebase(f);
        if (!table_fm.empty()) {
          CompressionStats cstats_fm;
          std::vector<Value> marked_fm = mark_dont_care_positions(table_fm, f, cstats_fm);
          CompressedTablebase ctb_fm = compress_tablebase(marked_fm, f);
          std::string filename_fm = tb_directory + "/" + compressed_tablebase_filename(f);
          save_compressed_tablebase(ctb_fm, filename_fm);

          BlockCompressionStats bstats_fm = analyze_block_compression(ctb_fm);
          std::cout << "  Saved " << filename_fm << " ("
                    << bstats_fm.compressed_size << " bytes, "
                    << std::fixed << std::setprecision(1) << bstats_fm.compression_ratio() << "x ratio)"
                    << std::endl;
        }
      }

      check_manager.clear();
    }
  }

  auto total_end = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end - total_start);
  std::cout << "\nTotal generation time: " << total_duration.count() << "s" << std::endl;
}

// Solve all DTM tablebases using position-level parallelism
// Materials are processed sequentially in dependency order, but each material
// uses all available cores for parallel position processing.
void solve_all_dtm_parallel(int max_pieces, bool load_existing = false) {
  std::vector<Material> all = all_materials(max_pieces);

  std::cout << "=== Parallel DTM Tablebase Generation ===" << std::endl;
  std::cout << "Using " << omp_get_max_threads() << " threads (position-level parallelism)" << std::endl;
  if (load_existing) {
    std::cout << "Mode: Load existing tablebases from disk when available" << std::endl;
  }
  std::cout << "Total materials: " << all.size() << std::endl;

  // Canonicalize materials
  std::unordered_map<Material, Material> canonical;
  std::unordered_set<Material> canonical_set;

  auto material_less = [](const Material& a, const Material& b) {
    if (a.back_white_pawns != b.back_white_pawns) return a.back_white_pawns < b.back_white_pawns;
    if (a.back_black_pawns != b.back_black_pawns) return a.back_black_pawns < b.back_black_pawns;
    if (a.other_white_pawns != b.other_white_pawns) return a.other_white_pawns < b.other_white_pawns;
    if (a.other_black_pawns != b.other_black_pawns) return a.other_black_pawns < b.other_black_pawns;
    if (a.white_queens != b.white_queens) return a.white_queens < b.white_queens;
    return a.black_queens < b.black_queens;
  };

  for (const Material& m : all) {
    Material f = flip(m);
    if (canonical.find(m) == canonical.end()) {
      Material c = material_less(m, f) ? m : f;
      canonical[m] = c;
      canonical[f] = c;
      canonical_set.insert(c);
    }
  }

  std::vector<Material> work_units(canonical_set.begin(), canonical_set.end());
  std::cout << "Work units (canonical materials): " << work_units.size() << std::endl;

  // Build dependency graph
  std::unordered_map<Material, std::unordered_set<Material>> deps_map;
  std::unordered_map<Material, int> dep_count;

  for (const Material& m : work_units) {
    std::vector<Material> deps = get_dependencies(m);
    std::unordered_set<Material> canonical_deps;
    for (const Material& dep : deps) {
      Material c = canonical[dep];
      if (!(c == m) && canonical_set.find(c) != canonical_set.end()) {
        canonical_deps.insert(c);
      }
    }
    deps_map[m] = canonical_deps;
    dep_count[m] = canonical_deps.size();
  }

  // Topological sort using Kahn's algorithm
  std::vector<Material> sorted;
  std::queue<Material> ready;

  for (const Material& m : work_units) {
    if (dep_count[m] == 0) {
      ready.push(m);
    }
  }

  while (!ready.empty()) {
    Material m = ready.front();
    ready.pop();
    sorted.push_back(m);

    for (const Material& other : work_units) {
      if (deps_map[other].count(m)) {
        dep_count[other]--;
        if (dep_count[other] == 0) {
          ready.push(other);
        }
      }
    }
  }

  if (sorted.size() != work_units.size()) {
    std::cerr << "ERROR: Dependency cycle detected in DTM generation!" << std::endl;
    return;
  }

  std::cout << std::endl;

  auto total_start = std::chrono::high_resolution_clock::now();

  // Process materials in dependency order
  for (const Material& m : sorted) {
    Material f = flip(m);

    // Try to load from disk if requested
    if (load_existing) {
      std::vector<DTM> tb_m = load_dtm(m);
      if (!tb_m.empty()) {
        store_dtm_tablebase(m, std::move(tb_m));
        if (!(f == m)) {
          std::vector<DTM> tb_f = load_dtm(f);
          if (!tb_f.empty()) {
            store_dtm_tablebase(f, std::move(tb_f));
          }
        }
        std::cout << "Loaded DTM " << m;
        if (!(f == m)) std::cout << " <-> " << f;
        std::cout << " from disk" << std::endl;
        continue;
      }
    }

    // Build dependency tablebases for this material
    std::vector<Material> deps = get_sub_materials(m);
    std::unordered_map<Material, std::vector<Value>> wdl_tables;
    std::unordered_map<Material, std::vector<DTM>> dtm_tables;

    for (const Material& dep : deps) {
      // Skip terminal materials
      if (dep.white_pieces() == 0 || dep.black_pieces() == 0) continue;

      // Load WDL
      std::vector<Value> wdl = get_wdl_tablebase(dep);
      if (wdl.empty()) wdl = load_tablebase(dep);
      if (!wdl.empty()) wdl_tables[dep] = std::move(wdl);

      // Load DTM
      std::vector<DTM> dtm = get_dtm_tablebase(dep);
      if (dtm.empty()) dtm = load_dtm(dep);
      if (!dtm.empty()) dtm_tables[dep] = std::move(dtm);
    }

    // Also load WDL for this material and its flip
    std::vector<Value> wdl_m = get_wdl_tablebase(m);
    if (wdl_m.empty()) wdl_m = load_tablebase(m);
    if (!wdl_m.empty()) wdl_tables[m] = std::move(wdl_m);

    if (!(f == m)) {
      std::vector<Value> wdl_f = get_wdl_tablebase(f);
      if (wdl_f.empty()) wdl_f = load_tablebase(f);
      if (!wdl_f.empty()) wdl_tables[f] = std::move(wdl_f);
    }

    // Generate using position-level parallelism
    generate_dtm_parallel(m, wdl_tables, dtm_tables);
  }

  auto total_end = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end - total_start);
  std::cout << "\nTotal DTM generation time: " << total_duration.count() << "s" << std::endl;
}

// Recursive solver with memoization
void solve(const Material& m,
           std::unordered_map<Material, std::vector<Value>>& tablebases) {

  // Skip if already solved
  if (tablebases.find(m) != tablebases.end()) {
    return;
  }

  // Handle terminal materials (one side has no pieces)
  if (m.white_pieces() == 0) {
    std::size_t size = material_size(m);
    if (size > 0) {
      std::vector<Value> table(size, Value::LOSS);
      tablebases[m] = table;
    }
    return;
  }
  if (m.black_pieces() == 0) {
    std::size_t size = material_size(m);
    if (size > 0) {
      std::vector<Value> table(size, Value::WIN);
      tablebases[m] = table;
    }
    return;
  }

  // First solve all dependencies (excluding flip(m))
  std::vector<Material> deps = get_dependencies(m);
  for (const Material& dep : deps) {
    solve(dep, tablebases);
  }

  // Check if m is self-symmetric
  Material flipped_m = flip(m);
  if (m == flipped_m) {
    // Self-symmetric - solve directly
    solve_single(m, tablebases);
  } else if (tablebases.find(flipped_m) == tablebases.end()) {
    // Non-symmetric and flip not yet solved - solve as pair
    solve_pair(m, flipped_m, tablebases);
  } else {
    // Non-symmetric but flip already solved - solve directly
    solve_single(m, tablebases);
  }
}

void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " [options] <back_w> <back_b> <other_w> <other_b> <queens_w> <queens_b>" << std::endl;
  std::cerr << "       " << program << " [options] --kvk     (generate queen vs queen)" << std::endl;
  std::cerr << "       " << program << " [options] --kvp     (generate queen vs pawn)" << std::endl;
  std::cerr << "       " << program << " [options] --all <n> (generate all with up to n pieces)" << std::endl;
  std::cerr << std::endl;
  std::cerr << "Options:" << std::endl;
  std::cerr << "  --dtm           Generate DTM (distance-to-mate) instead of WDL" << std::endl;
  std::cerr << "                  Requires WDL tablebases to exist first" << std::endl;
  std::cerr << "  --load-existing Load existing tablebases from disk instead of regenerating" << std::endl;
  std::cerr << "                  Useful for resuming interrupted generation" << std::endl;
  std::cerr << "  --compressed    Enable compressed-only mode for 8+ piece endgames" << std::endl;
  std::cerr << "                  Saves only cwdl_*.bin files, loads deps from compressed" << std::endl;
  std::cerr << "  --threshold N   Use compressed mode for N+ piece endgames (default 7)" << std::endl;
  std::cerr << "  --dir PATH      Output directory for tablebases (default: current dir)" << std::endl;
}

// Generate all material configurations with up to n total pieces
std::vector<Material> all_materials(int max_pieces) {
  std::vector<Material> result;

  for (int total = 2; total <= max_pieces; ++total) {
    // Enumerate all ways to distribute pieces
    // Note: bounds allow zero queens (all pieces can be pawns)
    for (int bwp = 0; bwp <= std::min(4, total); ++bwp) {
      for (int bbp = 0; bbp <= std::min(4, total - bwp); ++bbp) {
        for (int owp = 0; owp <= std::min(24, total - bwp - bbp); ++owp) {
          for (int obp = 0; obp <= std::min(24 - owp, total - bwp - bbp - owp); ++obp) {
            int remaining = total - bwp - bbp - owp - obp;
            for (int wq = 0; wq <= remaining; ++wq) {
              int bq = remaining - wq;
              if (bq >= 0) {
                Material m{bwp, bbp, owp, obp, wq, bq};
                // Must have at least one piece on each side
                if (m.white_pieces() > 0 && m.black_pieces() > 0) {
                  result.push_back(m);
                }
              }
            }
          }
        }
      }
    }
  }

  return result;
}

// ============================================================================
// DTM Generation
// ============================================================================

// Generate DTM tablebase for a single material configuration
// Requires WDL tablebase to exist
std::vector<DTM> generate_dtm(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
    const std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases) {

  std::size_t size = material_size(m);
  std::vector<DTM> dtm(size, DTM_UNKNOWN);
  std::vector<Move> moves;

  // Load WDL for this material
  auto wdl_it = wdl_tablebases.find(m);
  if (wdl_it == wdl_tablebases.end() || wdl_it->second.empty()) {
    std::cerr << "Error: WDL tablebase not found for " << m << std::endl;
    return {};
  }
  const std::vector<Value>& wdl = wdl_it->second;

  // Initialize: DRAW = 0, terminal LOSS = -32767, others = UNKNOWN
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (wdl[idx] == Value::DRAW) {
      dtm[idx] = DTM_DRAW;
    } else if (wdl[idx] == Value::LOSS) {
      // Check if this is a terminal LOSS (no moves)
      Board board = index_to_board(idx, m);
      generateMoves(board, moves);
      if (moves.empty()) {
        dtm[idx] = DTM_LOSS_TERMINAL;  // Terminal loss: -128
      }
      // Non-terminal losses will be computed via propagation
    }
  }

  // Build successor information for propagation
  // For each position, track which successors it has and their DTM values
  struct SuccessorInfo {
    std::size_t idx;
    Material mat;
    bool is_same_material;
  };

  std::vector<std::vector<SuccessorInfo>> successors(size);
  Material flipped_m = flip(m);

  for (std::size_t idx = 0; idx < size; ++idx) {
    if (wdl[idx] == Value::DRAW) continue;  // Skip draws

    Board board = index_to_board(idx, m);
    generateMoves(board, moves);

    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      Material next_m = get_material(next);

      SuccessorInfo info;
      info.mat = next_m;

      if (next_m == flipped_m) {
        // Same material after flip
        info.idx = board_to_index(next, flipped_m);
        info.is_same_material = (m == flipped_m);
      } else {
        // Different material (capture/promotion)
        info.idx = board_to_index(next, next_m);
        info.is_same_material = false;
      }

      successors[idx].push_back(info);
    }
  }

  // Iterate until stable (limit to 200 iterations for debugging)
  bool changed = true;
  int iteration = 0;
  constexpr int MAX_ITERATIONS = 200;
  while (changed && iteration < MAX_ITERATIONS) {
    changed = false;
    iteration++;

    for (std::size_t idx = 0; idx < size; ++idx) {
      Value val = wdl[idx];

      // Skip DRAW positions (already set) and positions without valid WDL
      if (val == Value::DRAW) continue;

      if (val == Value::WIN) {
        // Find opponent's quickest LOSS (highest negative DTM = fewest moves)
        // We want to win as fast as possible, so pick move where opponent
        // loses in the fewest moves. WIN in M+1 when opponent's best LOSS is M.
        // Note: DTM=-1 (loss in 1) > DTM=-2 (loss in 2), so we want HIGHEST.
        DTM best_succ = DTM_UNKNOWN;
        for (const SuccessorInfo& succ : successors[idx]) {
          DTM succ_dtm;

          // Check if capture resulted in opponent having 0 pieces (terminal loss)
          if (succ.mat.white_pieces() == 0 || succ.mat.black_pieces() == 0) {
            succ_dtm = DTM_LOSS_TERMINAL;
          } else if (succ.is_same_material) {
            succ_dtm = dtm[succ.idx];
          } else if (succ.mat == flipped_m) {
            auto it = dtm_tablebases.find(flipped_m);
            if (it != dtm_tablebases.end() && succ.idx < it->second.size()) {
              succ_dtm = it->second[succ.idx];
            } else {
              succ_dtm = DTM_UNKNOWN;
            }
          } else {
            auto it = dtm_tablebases.find(succ.mat);
            if (it != dtm_tablebases.end() && succ.idx < it->second.size()) {
              succ_dtm = it->second[succ.idx];
            } else {
              succ_dtm = DTM_UNKNOWN;
            }
          }

          // Losses are negative (< 0), excluding UNKNOWN
          if (succ_dtm < DTM_DRAW && succ_dtm != DTM_UNKNOWN) {
            // Track quickest loss for opponent (we want to win fast)
            // DTM_LOSS_TERMINAL (-128) = 0 moves, is best
            // Otherwise, highest (least negative) = fewest moves
            if (best_succ == DTM_UNKNOWN) {
              best_succ = succ_dtm;
            } else if (succ_dtm == DTM_LOSS_TERMINAL) {
              best_succ = DTM_LOSS_TERMINAL;  // Terminal is always fastest
            } else if (best_succ != DTM_LOSS_TERMINAL && succ_dtm > best_succ) {
              best_succ = succ_dtm;
            }
          }
        }

        if (best_succ != DTM_UNKNOWN) {
          // WIN in (opponent's loss moves + 1) moves
          int opp_moves = (best_succ == DTM_LOSS_TERMINAL) ? 0 : -best_succ;
          DTM new_dtm = dtm_win(opp_moves + 1);
          // Only update if unknown or if this is a faster win (smaller DTM)
          if (dtm[idx] == DTM_UNKNOWN || new_dtm < dtm[idx]) {
            dtm[idx] = new_dtm;
            changed = true;
          }
        }
      } else if (val == Value::LOSS) {
        // Find opponent's slowest WIN (highest positive DTM = most moves)
        // We want to survive as long as possible, so pick move where opponent
        // wins in the most moves. LOSS in M moves when opponent's best WIN is M.
        DTM best_succ = DTM_UNKNOWN;
        bool all_known = true;

        for (const SuccessorInfo& succ : successors[idx]) {
          DTM succ_dtm;

          // Check if capture resulted in one side having 0 pieces
          if (succ.mat.white_pieces() == 0) {
            // Side to move has 0 pieces = terminal loss for them = WIN for us
            // This shouldn't happen in a LOSS position, but handle gracefully
            succ_dtm = dtm_win(1);
          } else if (succ.mat.black_pieces() == 0) {
            // Opponent has 0 pieces = terminal loss for them
            succ_dtm = DTM_LOSS_TERMINAL;
          } else if (succ.is_same_material) {
            succ_dtm = dtm[succ.idx];
          } else if (succ.mat == flipped_m) {
            auto it = dtm_tablebases.find(flipped_m);
            if (it != dtm_tablebases.end() && succ.idx < it->second.size()) {
              succ_dtm = it->second[succ.idx];
            } else {
              succ_dtm = DTM_UNKNOWN;
            }
          } else {
            auto it = dtm_tablebases.find(succ.mat);
            if (it != dtm_tablebases.end() && succ.idx < it->second.size()) {
              succ_dtm = it->second[succ.idx];
            } else {
              succ_dtm = DTM_UNKNOWN;
            }
          }

          if (succ_dtm == DTM_UNKNOWN) {
            all_known = false;
            break;
          }

          // Wins are positive (> 0)
          if (succ_dtm > DTM_DRAW) {
            // Track maximum (largest WIN = most moves = longest survival)
            if (best_succ == DTM_UNKNOWN || succ_dtm > best_succ) {
              best_succ = succ_dtm;
            }
          }
        }

        if (all_known && best_succ != DTM_UNKNOWN) {
          // LOSS in same number of moves as opponent's WIN
          DTM new_dtm = dtm_loss(best_succ);
          if (dtm[idx] != new_dtm) {
            dtm[idx] = new_dtm;
            changed = true;
          }
        }
      }
    }

    if (iteration % 10 == 0) {
      std::size_t unknown_count = 0;
      for (std::size_t i = 0; i < size; ++i) {
        if (dtm[i] == DTM_UNKNOWN) unknown_count++;
      }
      std::cout << "    Iteration " << iteration << ": " << unknown_count << " unknown" << std::endl;
    }
  }

  if (iteration >= MAX_ITERATIONS) {
    std::cerr << "WARNING: Hit max iterations (" << MAX_ITERATIONS << "), possible bug!" << std::endl;
  }

  return dtm;
}

// Generate DTM for a material and its flip together (for non-symmetric materials)
void generate_dtm_pair(const Material& m1, const Material& m2,
                       const std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
                       std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases) {

  std::cout << "Generating DTM for pair " << m1 << " <-> " << m2 << std::endl;

  std::size_t size1 = material_size(m1);
  std::size_t size2 = material_size(m2);

  // Initialize with UNKNOWN
  std::vector<DTM> dtm1(size1, DTM_UNKNOWN);
  std::vector<DTM> dtm2(size2, DTM_UNKNOWN);

  // Set draws to 0
  auto wdl1_it = wdl_tablebases.find(m1);
  auto wdl2_it = wdl_tablebases.find(m2);
  if (wdl1_it == wdl_tablebases.end() || wdl2_it == wdl_tablebases.end()) {
    std::cerr << "Error: WDL not found for DTM generation" << std::endl;
    return;
  }

  const std::vector<Value>& wdl1 = wdl1_it->second;
  const std::vector<Value>& wdl2 = wdl2_it->second;

  std::vector<Move> moves;

  // Initialize terminals
  for (std::size_t idx = 0; idx < size1; ++idx) {
    if (wdl1[idx] == Value::DRAW) {
      dtm1[idx] = DTM_DRAW;
    } else if (wdl1[idx] == Value::LOSS) {
      Board board = index_to_board(idx, m1);
      generateMoves(board, moves);
      if (moves.empty()) dtm1[idx] = DTM_LOSS_TERMINAL;
    }
  }
  for (std::size_t idx = 0; idx < size2; ++idx) {
    if (wdl2[idx] == Value::DRAW) {
      dtm2[idx] = DTM_DRAW;
    } else if (wdl2[idx] == Value::LOSS) {
      Board board = index_to_board(idx, m2);
      generateMoves(board, moves);
      if (moves.empty()) dtm2[idx] = DTM_LOSS_TERMINAL;
    }
  }

  // Add to tablebases for lookups during iteration
  dtm_tablebases[m1] = dtm1;
  dtm_tablebases[m2] = dtm2;

  // Iterate (limit to 200 iterations for debugging)
  bool changed = true;
  int iteration = 0;
  constexpr int MAX_PAIR_ITERATIONS = 200;
  while (changed && iteration < MAX_PAIR_ITERATIONS) {
    changed = false;
    iteration++;

    // Generate DTM for m1
    std::vector<DTM> new_dtm1 = generate_dtm(m1, wdl_tablebases, dtm_tablebases);
    for (std::size_t i = 0; i < size1; ++i) {
      if (dtm1[i] != new_dtm1[i]) {
        changed = true;
        dtm1[i] = new_dtm1[i];
      }
    }
    dtm_tablebases[m1] = dtm1;

    // Generate DTM for m2
    std::vector<DTM> new_dtm2 = generate_dtm(m2, wdl_tablebases, dtm_tablebases);
    for (std::size_t i = 0; i < size2; ++i) {
      if (dtm2[i] != new_dtm2[i]) {
        changed = true;
        dtm2[i] = new_dtm2[i];
      }
    }
    dtm_tablebases[m2] = dtm2;

    std::cout << "  Iteration " << iteration << ": " << (changed ? "changed" : "stable") << std::endl;
  }

  if (iteration >= MAX_PAIR_ITERATIONS) {
    std::cerr << "WARNING: Hit max pair iterations (" << MAX_PAIR_ITERATIONS << "), possible bug!" << std::endl;
  }

  // Print statistics
  auto print_stats = [](const std::vector<DTM>& dtm, const Material& m) {
    int max_win_plies = 0, max_loss_plies = 0;
    std::size_t wins = 0, losses = 0, draws = 0, unknown = 0;
    for (DTM d : dtm) {
      if (d == DTM_DRAW) { draws++; }
      else if (d == DTM_UNKNOWN) { unknown++; }
      else if (d > 0) {
        wins++;
        int plies = dtm_to_plies(d);
        if (plies > max_win_plies) max_win_plies = plies;
      } else {
        losses++;  // d < 0: all losses (including terminal at -128)
        int plies = dtm_to_plies(d);
        if (plies > max_loss_plies) max_loss_plies = plies;
      }
    }
    std::cout << "  " << m << ": " << wins << " wins (max " << max_win_plies << " plies), "
              << losses << " losses (max " << max_loss_plies << " plies), "
              << draws << " draws";
    if (unknown > 0) std::cout << ", " << unknown << " unknown";
    std::cout << std::endl;
  };

  print_stats(dtm1, m1);
  print_stats(dtm2, m2);

  // Save
  save_dtm(dtm1, m1);
  save_dtm(dtm2, m2);
  std::cout << "  Saved to " << dtm_filename(m1) << " and " << dtm_filename(m2) << std::endl;
}

// Generate DTM for a single symmetric material
void generate_dtm_single(const Material& m,
                         const std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
                         std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases) {

  std::cout << "Generating DTM for " << m << " (" << material_size(m) << " positions)" << std::endl;

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<DTM> dtm = generate_dtm(m, wdl_tablebases, dtm_tablebases);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Statistics
  int max_win_plies = 0, max_loss_plies = 0;
  std::size_t wins = 0, losses = 0, draws = 0, unknown = 0;
  for (DTM d : dtm) {
    if (d == DTM_DRAW) { draws++; }
    else if (d == DTM_UNKNOWN) { unknown++; }
    else if (d > 0) {
      wins++;
      int plies = dtm_to_plies(d);
      if (plies > max_win_plies) max_win_plies = plies;
    } else {
      losses++;  // d < 0: all losses (including terminal)
      int plies = dtm_to_plies(d);
      if (plies > max_loss_plies) max_loss_plies = plies;
    }
  }

  std::cout << "  Completed in " << duration.count() << "ms" << std::endl;
  std::cout << "  Wins: " << wins << " (max " << max_win_plies << " plies)" << std::endl;
  std::cout << "  Losses: " << losses << " (max " << max_loss_plies << " plies)" << std::endl;
  std::cout << "  Draws: " << draws;
  if (unknown > 0) std::cout << ", Unknown: " << unknown;
  std::cout << std::endl;

  save_dtm(dtm, m);
  std::cout << "  Saved to " << dtm_filename(m) << std::endl;

  dtm_tablebases[m] = std::move(dtm);
}

// Solve DTM for a material (requires WDL to exist)
void solve_dtm(const Material& m,
               std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
               std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases) {

  // Skip if already solved
  if (dtm_tablebases.find(m) != dtm_tablebases.end()) {
    return;
  }

  // Handle terminal materials
  if (m.white_pieces() == 0 || m.black_pieces() == 0) {
    std::size_t size = material_size(m);
    if (size > 0) {
      // Terminal: all positions are DTM=0 (game over)
      std::vector<DTM> dtm(size, 0);
      dtm_tablebases[m] = dtm;
    }
    return;
  }

  // Load WDL if not already loaded
  if (wdl_tablebases.find(m) == wdl_tablebases.end()) {
    std::vector<Value> wdl = load_tablebase(m);
    if (wdl.empty()) {
      std::cerr << "Error: WDL tablebase not found for " << m << std::endl;
      std::cerr << "Run WDL generation first: ./generate --all <n>" << std::endl;
      return;
    }
    wdl_tablebases[m] = std::move(wdl);
  }

  // Solve dependencies first
  std::vector<Material> deps = get_dependencies(m);
  for (const Material& dep : deps) {
    solve_dtm(dep, wdl_tablebases, dtm_tablebases);
  }

  // Check symmetry
  Material flipped_m = flip(m);
  if (m == flipped_m) {
    // Self-symmetric
    generate_dtm_single(m, wdl_tablebases, dtm_tablebases);
  } else if (dtm_tablebases.find(flipped_m) == dtm_tablebases.end()) {
    // Need to load WDL for flipped too
    if (wdl_tablebases.find(flipped_m) == wdl_tablebases.end()) {
      std::vector<Value> wdl = load_tablebase(flipped_m);
      if (wdl.empty()) {
        std::cerr << "Error: WDL tablebase not found for " << flipped_m << std::endl;
        return;
      }
      wdl_tablebases[flipped_m] = std::move(wdl);
    }
    // Solve as pair
    generate_dtm_pair(m, flipped_m, wdl_tablebases, dtm_tablebases);
  } else {
    // Flip already solved
    generate_dtm_single(m, wdl_tablebases, dtm_tablebases);
  }
}

} // namespace

int main(int argc, char* argv[]) {
  // Parse option flags
  bool generate_dtm_flag = false;
  bool load_existing_flag = false;
  bool compressed_flag = false;
  int threshold = 7;  // Default: use compressed mode for 7+ pieces
  std::string tb_directory = ".";
  int arg_offset = 1;

  while (arg_offset < argc && argv[arg_offset][0] == '-' && argv[arg_offset][1] == '-') {
    if (std::strcmp(argv[arg_offset], "--dtm") == 0) {
      generate_dtm_flag = true;
      arg_offset++;
    } else if (std::strcmp(argv[arg_offset], "--load-existing") == 0) {
      load_existing_flag = true;
      arg_offset++;
    } else if (std::strcmp(argv[arg_offset], "--compressed") == 0) {
      compressed_flag = true;
      arg_offset++;
    } else if (std::strcmp(argv[arg_offset], "--threshold") == 0) {
      if (arg_offset + 1 >= argc) {
        std::cerr << "Error: --threshold requires a number" << std::endl;
        return 1;
      }
      threshold = std::atoi(argv[arg_offset + 1]);
      if (threshold < 2 || threshold > 8) {
        std::cerr << "Threshold must be between 2 and 8" << std::endl;
        return 1;
      }
      arg_offset += 2;
    } else if (std::strcmp(argv[arg_offset], "--dir") == 0) {
      if (arg_offset + 1 >= argc) {
        std::cerr << "Error: --dir requires a path" << std::endl;
        return 1;
      }
      tb_directory = argv[arg_offset + 1];
      arg_offset += 2;
    } else {
      break;  // Not an option flag, must be a command
    }
  }

  int remaining_argc = argc - arg_offset + 1;  // +1 to account for program name

  std::unordered_map<Material, std::vector<Value>> wdl_tablebases;
  std::unordered_map<Material, std::vector<DTM>> dtm_tablebases;

  if (remaining_argc == 2 && std::strcmp(argv[arg_offset], "--kvk") == 0) {
    // Queen vs Queen
    Material m{0, 0, 0, 0, 1, 1};
    if (generate_dtm_flag) {
      solve_dtm(m, wdl_tablebases, dtm_tablebases);
    } else {
      solve(m, wdl_tablebases);
    }
  } else if (remaining_argc == 2 && std::strcmp(argv[arg_offset], "--kvp") == 0) {
    // Queen vs Pawn (all variants)
    std::cout << "=== Queen vs Pawn ===" << std::endl;
    if (generate_dtm_flag) {
      solve_dtm(Material{0, 0, 0, 1, 1, 0}, wdl_tablebases, dtm_tablebases);
      solve_dtm(Material{0, 1, 0, 0, 1, 0}, wdl_tablebases, dtm_tablebases);
    } else {
      solve(Material{0, 0, 0, 1, 1, 0}, wdl_tablebases);
      solve(Material{0, 1, 0, 0, 1, 0}, wdl_tablebases);
    }
  } else if (remaining_argc == 3 && std::strcmp(argv[arg_offset], "--all") == 0) {
    int max_pieces = std::atoi(argv[arg_offset + 1]);
    if (max_pieces < 2 || max_pieces > 8) {
      std::cerr << "Max pieces must be between 2 and 8" << std::endl;
      return 1;
    }

    if (compressed_flag) {
      if (generate_dtm_flag) {
        std::cerr << "Error: --compressed mode does not support --dtm yet" << std::endl;
        return 1;
      }
      solve_all_parallel_compressed(max_pieces, threshold, tb_directory);
    } else if (generate_dtm_flag) {
      solve_all_dtm_parallel(max_pieces, load_existing_flag);
    } else {
      solve_all_parallel(max_pieces, load_existing_flag);
    }
  } else if (remaining_argc == 7) {
    Material m{
      std::atoi(argv[arg_offset]),
      std::atoi(argv[arg_offset + 1]),
      std::atoi(argv[arg_offset + 2]),
      std::atoi(argv[arg_offset + 3]),
      std::atoi(argv[arg_offset + 4]),
      std::atoi(argv[arg_offset + 5])
    };
    if (generate_dtm_flag) {
      solve_dtm(m, wdl_tablebases, dtm_tablebases);
    } else {
      solve(m, wdl_tablebases);
    }
  } else {
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
