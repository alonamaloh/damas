#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
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

// Build local sub_tablebases from dependencies (for thread-local use)
std::unordered_map<Material, std::vector<Value>> build_local_wdl_tablebases(
    const std::vector<Material>& deps) {
  std::unordered_map<Material, std::vector<Value>> local;
  for (const Material& dep : deps) {
    std::vector<Value> tb = get_wdl_tablebase(dep);
    if (!tb.empty()) {
      local[dep] = std::move(tb);
    }
  }
  return local;
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

std::unordered_map<Material, std::vector<DTM>> build_local_dtm_tablebases(
    const std::vector<Material>& deps) {
  std::unordered_map<Material, std::vector<DTM>> local;
  for (const Material& dep : deps) {
    std::vector<DTM> tb = get_dtm_tablebase(dep);
    if (!tb.empty()) {
      local[dep] = std::move(tb);
    }
  }
  return local;
}

// Forward declarations for parallel solver
std::vector<Material> all_materials(int max_pieces);
std::vector<DTM> generate_dtm(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& wdl_tablebases,
    const std::unordered_map<Material, std::vector<DTM>>& dtm_tablebases);

// Check if a position has captures available (mandatory in Spanish checkers)
bool has_captures(const Board& board) {
  Bb empty = board.empty();

  // Pawn captures
  Bb pawnCanCaptureNW = moveSE(board.black & moveSE(empty));
  Bb pawnCanCaptureNE = moveSW(board.black & moveSW(empty));
  if (board.whitePawns() & (pawnCanCaptureNW | pawnCanCaptureNE)) {
    return true;
  }

  // Queen captures
  Bb kingOrEmpty = board.whiteQueens() | empty;
  Bb canBeCapturedNW = board.black & moveSE(kingOrEmpty);
  Bb canBeCapturedNE = board.black & moveSW(kingOrEmpty);
  Bb canBeCapturedSE = board.black & moveNW(kingOrEmpty);
  Bb canBeCapturedSW = board.black & moveNE(kingOrEmpty);

  // Check if any queen can reach a capture position
  Bb queenCanCapture = 0;
  for (Bb x = moveSE(canBeCapturedNW) & kingOrEmpty; x; x = moveSE(x) & kingOrEmpty) {
    queenCanCapture |= x;
    x &= empty;
  }
  for (Bb x = moveSW(canBeCapturedNE) & kingOrEmpty; x; x = moveSW(x) & kingOrEmpty) {
    queenCanCapture |= x;
    x &= empty;
  }
  for (Bb x = moveNW(canBeCapturedSE) & kingOrEmpty; x; x = moveNW(x) & kingOrEmpty) {
    queenCanCapture |= x;
    x &= empty;
  }
  for (Bb x = moveNE(canBeCapturedSW) & kingOrEmpty; x; x = moveNE(x) & kingOrEmpty) {
    queenCanCapture |= x;
    x &= empty;
  }

  return (board.whiteQueens() & queenCanCapture) != 0;
}

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

// Generate tablebase for a single material configuration
// Requires all sub-tablebases (reachable by captures/promotions) to be loaded
std::vector<Value> generate_tablebase(
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& sub_tablebases,
    Stats& stats) {

  std::size_t size = material_size(m);
  stats.total_positions = size;

  std::vector<Value> table(size, Value::UNKNOWN);
  std::vector<Move> moves;

  // Count remaining unknown positions for each index (for LOSS detection)
  // A position is LOSS if ALL successors are WIN for opponent
  std::vector<std::uint16_t> unknown_successor_count(size, 0);

  // Track positions with external draw escapes (material-changing moves to DRAW)
  // If a position has an external draw escape, it can't be LOSS even if all
  // internal successors are WIN for opponent
  std::vector<bool> has_external_draw(size, false);

  // Predecessor lists for backward propagation
  // predecessors[i] = list of (predecessor_index) that lead to position i
  std::vector<std::vector<std::size_t>> predecessors(size);

  // Phase 1: Initialize terminal positions and build predecessor graph
  for (std::size_t idx = 0; idx < size; ++idx) {
    Board board = index_to_board(idx, m);

    // Generate moves from this position
    generateMoves(board, moves);

    if (moves.empty()) {
      // No moves = side to move loses
      table[idx] = Value::LOSS;
      stats.losses++;
      continue;
    }

    bool position_has_captures = has_captures(board);
    if (position_has_captures) {
      stats.with_captures++;
    }

    // Check successors
    std::size_t successor_count = 0;
    std::size_t bad_successors = 0;  // Successors where opponent wins
    bool found_win = false;

    for (const Move& move : moves) {
      Board next = makeMove(board, move);

      // After makeMove, board is flipped (opponent's turn)
      Material next_m = get_material(next);

      // Check if material changed (capture or promotion)
      bool material_changed = !(next_m == flip(m));

      if (material_changed) {
        // Check for terminal positions first (one side eliminated)
        if (next_m.white_pieces() == 0) {
          // After flip, opponent (original us) has 0 pieces = we captured everything
          // Opponent loses immediately = we win!
          found_win = true;
        } else if (next_m.black_pieces() == 0) {
          // After flip, we (original opponent) have 0 pieces = shouldn't happen
          // This would mean we captured our own pieces, which is impossible
          bad_successors++;
        } else {
          // Lookup in sub-tablebase (from opponent's perspective)
          // After flip in makeMove, 'next' has opponent as "white" (to move)
          // We lookup from their perspective
          auto it = sub_tablebases.find(next_m);
          if (it != sub_tablebases.end() && !it->second.empty()) {
          // Verify material matches before indexing
          Material verify_m = get_material(next);
          if (verify_m == next_m) {
            std::size_t tb_size = it->second.size();
            std::size_t expected_size = material_size(next_m);
            if (tb_size == expected_size) {
              std::size_t next_idx = board_to_index(next, next_m);
              if (next_idx < tb_size) {
                Value sub_val = it->second[next_idx];
                if (sub_val == Value::LOSS) {
                  // Opponent (to move next) loses -> we win!
                  found_win = true;
                } else if (sub_val == Value::WIN) {
                  // Opponent wins -> this successor is bad for us
                  bad_successors++;
                } else if (sub_val == Value::DRAW) {
                  // External draw escape - can't be LOSS even if all internal moves are bad
                  has_external_draw[idx] = true;
                }
                // UNKNOWN doesn't resolve the position yet
              }
            }
          }
        }
        }
      } else {
        // Quiet move - material stays same after flip
        // If M == flip(M), we can use predecessor links for efficient propagation
        // Otherwise, we need to treat this as a sub-tablebase lookup
        Material flipped_m = flip(m);
        if (m == flipped_m) {
          // Self-symmetric material - build predecessor link
          std::size_t next_idx = board_to_index(next, m);
          if (next_idx < size) {
            predecessors[next_idx].push_back(idx);
            successor_count++;
          }
        } else {
          // Non-symmetric material - lookup in flip(M)'s tablebase
          auto it = sub_tablebases.find(flipped_m);
          if (it != sub_tablebases.end() && !it->second.empty()) {
            std::size_t next_idx = board_to_index(next, flipped_m);
            if (next_idx < it->second.size()) {
              Value sub_val = it->second[next_idx];
              if (sub_val == Value::LOSS) {
                found_win = true;
              } else if (sub_val == Value::WIN) {
                bad_successors++;
              } else if (sub_val == Value::DRAW) {
                // External draw escape
                has_external_draw[idx] = true;
              }
            }
          }
        }
      }
    }

    if (found_win) {
      table[idx] = Value::WIN;
      stats.wins++;
    } else if (bad_successors == moves.size()) {
      // All moves lead to opponent WIN -> we lose
      table[idx] = Value::LOSS;
      stats.losses++;
    } else if (successor_count == 0) {
      // No unknown successors and no wins -> must be all draws/unknown from sub-tables
      // For non-symmetric materials, this means DRAW
      table[idx] = Value::DRAW;
      stats.draws++;
    } else {
      // Some successors need propagation (only for self-symmetric materials)
      unknown_successor_count[idx] = successor_count;
    }
  }

  // Phase 2: Propagate using work queue
  std::queue<std::size_t> work_queue;

  // Initialize work queue with known losses (positions where opponent is to move
  // and can reach us means we might be a WIN)
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (table[idx] == Value::LOSS || table[idx] == Value::WIN) {
      work_queue.push(idx);
    }
  }

  while (!work_queue.empty()) {
    std::size_t idx = work_queue.front();
    work_queue.pop();

    Value val = table[idx];
    if (val != Value::WIN && val != Value::LOSS) continue;

    // Propagate to predecessors
    for (std::size_t pred_idx : predecessors[idx]) {
      if (table[pred_idx] != Value::UNKNOWN) continue;

      if (val == Value::LOSS) {
        // Successor is LOSS for opponent -> we WIN
        table[pred_idx] = Value::WIN;
        stats.wins++;
        work_queue.push(pred_idx);
      } else {  // val == Value::WIN
        // Successor is WIN for opponent -> one fewer good option for us
        unknown_successor_count[pred_idx]--;
        if (unknown_successor_count[pred_idx] == 0) {
          // All internal successors are wins for opponent
          if (has_external_draw[pred_idx]) {
            // But we have an external draw escape -> DRAW
            table[pred_idx] = Value::DRAW;
            stats.draws++;
            // Don't push to work queue - DRAW doesn't propagate
          } else {
            // No escape -> we LOSE
            table[pred_idx] = Value::LOSS;
            stats.losses++;
            work_queue.push(pred_idx);
          }
        }
      }
    }
  }

  // Phase 3: Remaining UNKNOWN positions are DRAWs
  for (std::size_t idx = 0; idx < size; ++idx) {
    if (table[idx] == Value::UNKNOWN) {
      table[idx] = Value::DRAW;
      stats.draws++;
    }
  }

  return table;
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

// Thread-safe solve for a single symmetric material
// Uses global g_wdl_tablebases and builds local copies of dependencies
void solve_single_threadsafe(const Material& m) {
  // Build local sub_tablebases from all reachable materials (for multi-captures)
  std::vector<Material> deps = get_sub_materials(m);
  std::unordered_map<Material, std::vector<Value>> local_tablebases = build_local_wdl_tablebases(deps);

  // Also load from disk if not in memory
  for (const Material& dep : deps) {
    if (local_tablebases.find(dep) == local_tablebases.end()) {
      std::vector<Value> tb = load_tablebase(dep);
      if (!tb.empty()) {
        local_tablebases[dep] = std::move(tb);
      }
    }
  }

  auto start = std::chrono::high_resolution_clock::now();

  Stats stats;
  std::vector<Value> table = generate_tablebase(m, local_tablebases, stats);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Thread-safe output
  #pragma omp critical
  {
    std::cout << "[Thread " << omp_get_thread_num() << "] " << m
              << " (" << material_size(m) << " pos): "
              << stats.wins << "W/" << stats.losses << "L/" << stats.draws << "D in "
              << duration.count() << "ms" << std::endl;
  }

  save_tablebase(table, m);
  store_wdl_tablebase(m, std::move(table));
}

// Thread-safe solve for a pair of mutually-dependent materials
void solve_pair_threadsafe(const Material& m1, const Material& m2) {
  // Build local sub_tablebases from all reachable materials (for multi-captures)
  std::vector<Material> deps = get_sub_materials(m1);  // Same as m2's deps
  std::unordered_map<Material, std::vector<Value>> local_tablebases = build_local_wdl_tablebases(deps);

  // Also load from disk if not in memory
  for (const Material& dep : deps) {
    if (local_tablebases.find(dep) == local_tablebases.end()) {
      std::vector<Value> tb = load_tablebase(dep);
      if (!tb.empty()) {
        local_tablebases[dep] = std::move(tb);
      }
    }
  }

  std::size_t size1 = material_size(m1);
  std::size_t size2 = material_size(m2);

  // Initialize both tables
  std::vector<Value> table1(size1, Value::DRAW);
  std::vector<Value> table2(size2, Value::DRAW);

  local_tablebases[m1] = table1;
  local_tablebases[m2] = table2;

  auto start = std::chrono::high_resolution_clock::now();

  // Iterate until stable
  bool changed = true;
  int iteration = 0;
  while (changed && iteration < 100) {
    changed = false;
    iteration++;

    Stats stats1;
    std::vector<Value> new_table1 = generate_tablebase(m1, local_tablebases, stats1);
    for (std::size_t i = 0; i < size1; ++i) {
      if (table1[i] != new_table1[i]) {
        changed = true;
        table1[i] = new_table1[i];
      }
    }
    local_tablebases[m1] = table1;

    Stats stats2;
    std::vector<Value> new_table2 = generate_tablebase(m2, local_tablebases, stats2);
    for (std::size_t i = 0; i < size2; ++i) {
      if (table2[i] != new_table2[i]) {
        changed = true;
        table2[i] = new_table2[i];
      }
    }
    local_tablebases[m2] = table2;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Count statistics
  std::size_t wins1 = 0, losses1 = 0, draws1 = 0;
  for (Value v : table1) {
    if (v == Value::WIN) wins1++;
    else if (v == Value::LOSS) losses1++;
    else draws1++;
  }
  std::size_t wins2 = 0, losses2 = 0, draws2 = 0;
  for (Value v : table2) {
    if (v == Value::WIN) wins2++;
    else if (v == Value::LOSS) losses2++;
    else draws2++;
  }

  #pragma omp critical
  {
    std::cout << "[Thread " << omp_get_thread_num() << "] Pair " << m1 << " <-> " << m2
              << " (" << iteration << " iters, " << duration.count() << "ms):" << std::endl;
    std::cout << "    " << m1 << ": " << wins1 << "W/" << losses1 << "L/" << draws1 << "D" << std::endl;
    std::cout << "    " << m2 << ": " << wins2 << "W/" << losses2 << "L/" << draws2 << "D" << std::endl;
  }

  save_tablebase(table1, m1);
  save_tablebase(table2, m2);
  store_wdl_tablebase(m1, std::move(table1));
  store_wdl_tablebase(m2, std::move(table2));
}

// Solve all materials up to max_pieces in parallel using dependency graph
// A material can start processing as soon as all its actual dependencies are done,
// not when all materials at a "lower level" are done.
// If load_existing is true, try to load tablebases from disk instead of generating.
void solve_all_parallel(int max_pieces, bool load_existing = false) {
  std::vector<Material> all = all_materials(max_pieces);

  std::cout << "=== Parallel Tablebase Generation ===" << std::endl;
  std::cout << "Using " << omp_get_max_threads() << " threads" << std::endl;
  if (load_existing) {
    std::cout << "Mode: Load existing tablebases from disk when available" << std::endl;
  }
  std::cout << "Total materials: " << all.size() << std::endl;

  // Canonicalize materials: for asymmetric pairs, pick one representative
  // Map each material to its canonical form
  std::unordered_map<Material, Material> canonical;
  std::unordered_set<Material> canonical_set;

  // Helper to pick canonical representative (lexicographic on fields)
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
      // Use smaller material as canonical (arbitrary but consistent)
      Material c = material_less(m, f) ? m : f;
      canonical[m] = c;
      canonical[f] = c;
      canonical_set.insert(c);
    }
  }

  std::vector<Material> work_units(canonical_set.begin(), canonical_set.end());
  std::cout << "Work units (canonical materials): " << work_units.size() << std::endl << std::endl;

  // Build dependency graph: for each work unit, which work units must complete first?
  std::unordered_map<Material, std::vector<Material>> dependents;  // m -> list of materials that depend on m
  std::unordered_map<Material, int> dep_count;  // m -> number of unresolved dependencies

  for (const Material& m : work_units) {
    dep_count[m] = 0;
  }

  for (const Material& m : work_units) {
    // Get dependencies for both m and flip(m)
    std::vector<Material> deps = get_dependencies(m);
    std::unordered_set<Material> canonical_deps;

    for (const Material& dep : deps) {
      Material c = canonical[dep];
      if (c != m && canonical_set.find(c) != canonical_set.end()) {
        canonical_deps.insert(c);
      }
    }

    dep_count[m] = canonical_deps.size();
    for (const Material& dep : canonical_deps) {
      dependents[dep].push_back(m);
    }
  }

  // Initialize terminal materials and work queue
  std::queue<Material> ready_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  int remaining = work_units.size();
  bool done = false;

  for (const Material& m : work_units) {
    // Handle terminal materials immediately
    if (m.white_pieces() == 0 || m.black_pieces() == 0) {
      std::size_t size = material_size(m);
      if (size > 0) {
        Value terminal_val = (m.white_pieces() == 0) ? Value::LOSS : Value::WIN;
        std::vector<Value> table(size, terminal_val);
        store_wdl_tablebase(m, std::move(table));
      }
      // Also store flipped version
      Material f = flip(m);
      if (!(f == m)) {
        std::size_t fsize = material_size(f);
        if (fsize > 0) {
          Value terminal_val = (f.white_pieces() == 0) ? Value::LOSS : Value::WIN;
          std::vector<Value> table(fsize, terminal_val);
          store_wdl_tablebase(f, std::move(table));
        }
      }
      // Mark complete and update dependents
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        remaining--;
        for (const Material& dep_m : dependents[m]) {
          dep_count[dep_m]--;
          if (dep_count[dep_m] == 0) {
            ready_queue.push(dep_m);
          }
        }
      }
    } else if (dep_count[m] == 0) {
      ready_queue.push(m);
    }
  }

  auto total_start = std::chrono::high_resolution_clock::now();

  // Worker function
  auto worker = [&]() {
    while (true) {
      Material m;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [&]() { return !ready_queue.empty() || done; });
        if (done && ready_queue.empty()) return;
        if (ready_queue.empty()) continue;
        m = ready_queue.front();
        ready_queue.pop();
      }

      // Process this material
      Material f = flip(m);
      bool loaded_from_disk = false;

      if (load_existing) {
        // Try to load from disk
        std::vector<Value> tb_m = load_tablebase(m);
        if (!tb_m.empty()) {
          store_wdl_tablebase(m, std::move(tb_m));
          if (!(f == m)) {
            std::vector<Value> tb_f = load_tablebase(f);
            if (!tb_f.empty()) {
              store_wdl_tablebase(f, std::move(tb_f));
              loaded_from_disk = true;
              #pragma omp critical
              std::cout << "[Thread " << omp_get_thread_num() << "] Loaded " << m << " <-> " << f << " from disk" << std::endl;
            }
          } else {
            loaded_from_disk = true;
            #pragma omp critical
            std::cout << "[Thread " << omp_get_thread_num() << "] Loaded " << m << " from disk" << std::endl;
          }
        }
      }

      if (!loaded_from_disk) {
        if (m == f) {
          solve_single_threadsafe(m);
        } else {
          solve_pair_threadsafe(m, f);
        }
      }

      // Update dependents
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        remaining--;
        for (const Material& dep_m : dependents[m]) {
          dep_count[dep_m]--;
          if (dep_count[dep_m] == 0) {
            ready_queue.push(dep_m);
          }
        }
        if (remaining == 0) {
          done = true;
        }
        queue_cv.notify_all();
      }
    }
  };

  // Launch worker threads
  int num_threads = omp_get_max_threads();
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker);
  }

  // Wait for all to complete
  for (auto& t : threads) {
    t.join();
  }

  auto total_end = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(total_end - total_start);
  std::cout << "\nTotal generation time: " << total_duration.count() << "s" << std::endl;
}

// Thread-safe DTM solve for a single (self-symmetric) material
void solve_dtm_single_threadsafe(const Material& m) {
  // Build local copies of dependencies
  std::vector<Material> deps = get_sub_materials(m);
  std::unordered_map<Material, std::vector<Value>> local_wdl = build_local_wdl_tablebases(deps);
  std::unordered_map<Material, std::vector<DTM>> local_dtm = build_local_dtm_tablebases(deps);

  // Also load from disk if not in memory
  for (const Material& dep : deps) {
    if (local_wdl.find(dep) == local_wdl.end()) {
      std::vector<Value> tb = load_tablebase(dep);
      if (!tb.empty()) local_wdl[dep] = std::move(tb);
    }
    if (local_dtm.find(dep) == local_dtm.end()) {
      std::vector<DTM> tb = load_dtm(dep);
      if (!tb.empty()) local_dtm[dep] = std::move(tb);
    }
  }

  // Load WDL for this material
  std::vector<Value> wdl = get_wdl_tablebase(m);
  if (wdl.empty()) {
    wdl = load_tablebase(m);
    if (wdl.empty()) {
      #pragma omp critical
      std::cerr << "Error: WDL not found for " << m << std::endl;
      return;
    }
  }
  local_wdl[m] = wdl;

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<DTM> dtm = generate_dtm(m, local_wdl, local_dtm);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Statistics
  int max_win_plies = 0, max_loss_plies = 0;
  std::size_t wins = 0, losses = 0, draws = 0;
  for (DTM d : dtm) {
    if (d == DTM_DRAW) draws++;
    else if (d > 0) {
      wins++;
      int plies = dtm_to_plies(d);
      if (plies > max_win_plies) max_win_plies = plies;
    } else if (d != DTM_UNKNOWN) {
      losses++;
      int plies = dtm_to_plies(d);
      if (plies > max_loss_plies) max_loss_plies = plies;
    }
  }

  #pragma omp critical
  {
    std::cout << "[Thread " << omp_get_thread_num() << "] DTM " << m
              << ": " << wins << "W/" << losses << "L/" << draws << "D"
              << " (max " << max_win_plies << "/" << max_loss_plies << " plies) in "
              << duration.count() << "ms" << std::endl;
  }

  save_dtm(dtm, m);
  store_dtm_tablebase(m, std::move(dtm));
}

// Thread-safe DTM solve for a pair of mutually-dependent materials
void solve_dtm_pair_threadsafe(const Material& m1, const Material& m2) {
  // Build local copies of dependencies
  std::vector<Material> deps = get_sub_materials(m1);
  std::unordered_map<Material, std::vector<Value>> local_wdl = build_local_wdl_tablebases(deps);
  std::unordered_map<Material, std::vector<DTM>> local_dtm = build_local_dtm_tablebases(deps);

  // Also load from disk if not in memory
  for (const Material& dep : deps) {
    if (local_wdl.find(dep) == local_wdl.end()) {
      std::vector<Value> tb = load_tablebase(dep);
      if (!tb.empty()) local_wdl[dep] = std::move(tb);
    }
    if (local_dtm.find(dep) == local_dtm.end()) {
      std::vector<DTM> tb = load_dtm(dep);
      if (!tb.empty()) local_dtm[dep] = std::move(tb);
    }
  }

  // Load WDL for both materials
  std::vector<Value> wdl1 = get_wdl_tablebase(m1);
  if (wdl1.empty()) wdl1 = load_tablebase(m1);
  std::vector<Value> wdl2 = get_wdl_tablebase(m2);
  if (wdl2.empty()) wdl2 = load_tablebase(m2);

  if (wdl1.empty() || wdl2.empty()) {
    #pragma omp critical
    std::cerr << "Error: WDL not found for " << m1 << " or " << m2 << std::endl;
    return;
  }
  local_wdl[m1] = wdl1;
  local_wdl[m2] = wdl2;

  std::size_t size1 = material_size(m1);
  std::size_t size2 = material_size(m2);

  // Initialize with UNKNOWN
  std::vector<DTM> dtm1(size1, DTM_UNKNOWN);
  std::vector<DTM> dtm2(size2, DTM_UNKNOWN);

  // Set draws and terminal losses
  std::vector<Move> moves;
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

  local_dtm[m1] = dtm1;
  local_dtm[m2] = dtm2;

  auto start = std::chrono::high_resolution_clock::now();

  // Iterate until stable
  bool changed = true;
  int iteration = 0;
  while (changed && iteration < 200) {
    changed = false;
    iteration++;

    std::vector<DTM> new_dtm1 = generate_dtm(m1, local_wdl, local_dtm);
    for (std::size_t i = 0; i < size1; ++i) {
      if (dtm1[i] != new_dtm1[i]) {
        changed = true;
        dtm1[i] = new_dtm1[i];
      }
    }
    local_dtm[m1] = dtm1;

    std::vector<DTM> new_dtm2 = generate_dtm(m2, local_wdl, local_dtm);
    for (std::size_t i = 0; i < size2; ++i) {
      if (dtm2[i] != new_dtm2[i]) {
        changed = true;
        dtm2[i] = new_dtm2[i];
      }
    }
    local_dtm[m2] = dtm2;
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

  auto [w1, l1, d1, mw1, ml1] = count_stats(dtm1);
  auto [w2, l2, d2, mw2, ml2] = count_stats(dtm2);

  #pragma omp critical
  {
    std::cout << "[Thread " << omp_get_thread_num() << "] DTM pair " << m1 << " <-> " << m2
              << " (" << iteration << " iters, " << duration.count() << "ms):" << std::endl;
    std::cout << "    " << m1 << ": " << w1 << "W/" << l1 << "L/" << d1 << "D"
              << " (max " << mw1 << "/" << ml1 << " plies)" << std::endl;
    std::cout << "    " << m2 << ": " << w2 << "W/" << l2 << "L/" << d2 << "D"
              << " (max " << mw2 << "/" << ml2 << " plies)" << std::endl;
  }

  save_dtm(dtm1, m1);
  save_dtm(dtm2, m2);
  store_dtm_tablebase(m1, std::move(dtm1));
  store_dtm_tablebase(m2, std::move(dtm2));
}

// Solve all DTM tablebases in parallel using dependency graph
void solve_all_dtm_parallel(int max_pieces, bool load_existing = false) {
  std::vector<Material> all = all_materials(max_pieces);

  std::cout << "=== Parallel DTM Tablebase Generation ===" << std::endl;
  std::cout << "Using " << omp_get_max_threads() << " threads" << std::endl;
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
  std::cout << "Work units (canonical materials): " << work_units.size() << std::endl << std::endl;

  // Build dependency graph
  std::unordered_map<Material, std::vector<Material>> dependents;
  std::unordered_map<Material, int> dep_count;

  for (const Material& m : work_units) {
    dep_count[m] = 0;
  }

  for (const Material& m : work_units) {
    std::vector<Material> deps = get_dependencies(m);
    std::unordered_set<Material> canonical_deps;

    for (const Material& dep : deps) {
      Material c = canonical[dep];
      if (c != m && canonical_set.find(c) != canonical_set.end()) {
        canonical_deps.insert(c);
      }
    }

    dep_count[m] = canonical_deps.size();
    for (const Material& dep : canonical_deps) {
      dependents[dep].push_back(m);
    }
  }

  // Initialize ready queue
  std::queue<Material> ready_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  int remaining = work_units.size();
  bool done = false;

  for (const Material& m : work_units) {
    // Handle terminal materials
    if (m.white_pieces() == 0 || m.black_pieces() == 0) {
      std::size_t size = material_size(m);
      if (size > 0) {
        std::vector<DTM> dtm(size, DTM_DRAW);  // Terminal = game over
        store_dtm_tablebase(m, std::move(dtm));
      }
      Material f = flip(m);
      if (!(f == m)) {
        std::size_t fsize = material_size(f);
        if (fsize > 0) {
          std::vector<DTM> dtm(fsize, DTM_DRAW);
          store_dtm_tablebase(f, std::move(dtm));
        }
      }
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        remaining--;
        for (const Material& dep_m : dependents[m]) {
          dep_count[dep_m]--;
          if (dep_count[dep_m] == 0) {
            ready_queue.push(dep_m);
          }
        }
      }
    } else if (dep_count[m] == 0) {
      ready_queue.push(m);
    }
  }

  auto total_start = std::chrono::high_resolution_clock::now();

  // Worker function
  auto worker = [&]() {
    while (true) {
      Material m;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [&]() { return !ready_queue.empty() || done; });
        if (done && ready_queue.empty()) return;
        if (ready_queue.empty()) continue;
        m = ready_queue.front();
        ready_queue.pop();
      }

      Material f = flip(m);
      bool loaded_from_disk = false;

      if (load_existing) {
        std::vector<DTM> tb_m = load_dtm(m);
        if (!tb_m.empty()) {
          store_dtm_tablebase(m, std::move(tb_m));
          if (!(f == m)) {
            std::vector<DTM> tb_f = load_dtm(f);
            if (!tb_f.empty()) {
              store_dtm_tablebase(f, std::move(tb_f));
              loaded_from_disk = true;
              #pragma omp critical
              std::cout << "[Thread " << omp_get_thread_num() << "] Loaded DTM " << m << " <-> " << f << " from disk" << std::endl;
            }
          } else {
            loaded_from_disk = true;
            #pragma omp critical
            std::cout << "[Thread " << omp_get_thread_num() << "] Loaded DTM " << m << " from disk" << std::endl;
          }
        }
      }

      if (!loaded_from_disk) {
        if (m == f) {
          solve_dtm_single_threadsafe(m);
        } else {
          solve_dtm_pair_threadsafe(m, f);
        }
      }

      // Update dependents
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        remaining--;
        for (const Material& dep_m : dependents[m]) {
          dep_count[dep_m]--;
          if (dep_count[dep_m] == 0) {
            ready_queue.push(dep_m);
          }
        }
        if (remaining == 0) {
          done = true;
        }
        queue_cv.notify_all();
      }
    }
  };

  // Launch worker threads
  int num_threads = omp_get_max_threads();
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker);
  }

  for (auto& t : threads) {
    t.join();
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
  int arg_offset = 1;

  while (arg_offset < argc && argv[arg_offset][0] == '-' && argv[arg_offset][1] == '-') {
    if (std::strcmp(argv[arg_offset], "--dtm") == 0) {
      generate_dtm_flag = true;
      arg_offset++;
    } else if (std::strcmp(argv[arg_offset], "--load-existing") == 0) {
      load_existing_flag = true;
      arg_offset++;
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

    if (generate_dtm_flag) {
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
