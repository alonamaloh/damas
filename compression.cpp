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
