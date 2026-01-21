#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <unordered_map>

// Cache for looking up successors
std::unordered_map<Material, std::vector<Value>> wdl_cache;

Value lookup_wdl(const Board& b) {
  Material m = get_material(b);
  if (wdl_cache.find(m) == wdl_cache.end()) {
    wdl_cache[m] = load_tablebase(m);
  }
  auto& tb = wdl_cache[m];
  if (tb.empty()) return Value::UNKNOWN;
  std::size_t idx = board_to_index(b, m);
  if (idx >= tb.size()) return Value::UNKNOWN;
  return tb[idx];
}

int main() {
  // Investigate error at index 17506 of material 001111
  // White: 0x1000008, Black: 0x2100000, Kings: 0x100008
  // Expected: LOSS, Got: DRAW

  Board b;
  b.white = 0x1000008;
  b.black = 0x2100000;
  b.kings = 0x100008;
  b.n_reversible = 0;

  std::cout << "Board:\n" << b << std::endl;

  Material m = get_material(b);
  std::cout << "Material: " << m << std::endl;
  std::cout << "Material size: " << material_size(m) << std::endl;

  std::size_t idx = board_to_index(b, m);
  std::cout << "Index: " << idx << std::endl;

  // Load WDL
  std::vector<Value> wdl = load_tablebase(m);
  if (wdl.empty()) {
    std::cout << "WDL tablebase not found" << std::endl;
    return 1;
  }
  std::cout << "WDL value: ";
  switch (wdl[idx]) {
    case Value::WIN: std::cout << "WIN"; break;
    case Value::LOSS: std::cout << "LOSS"; break;
    case Value::DRAW: std::cout << "DRAW"; break;
    default: std::cout << "UNKNOWN"; break;
  }
  std::cout << std::endl;

  // Generate moves
  std::vector<Move> moves;
  generateMoves(b, moves);
  std::cout << "\nMoves available: " << moves.size() << std::endl;

  // Analyze all successors
  std::cout << "\n=== All successor positions ===" << std::endl;
  Material flipped_m = flip(m);
  std::cout << "Flipped material (for quiet moves): " << flipped_m << std::endl;

  bool found_loss = false;  // If we find opponent LOSS, we should be WIN
  bool all_wins = true;     // If all successors are opponent WIN, we should be LOSS

  for (size_t i = 0; i < moves.size(); i++) {
    Board next = makeMove(b, moves[i]);
    Material next_m = get_material(next);
    Value succ_val = lookup_wdl(next);

    // Check for terminal capture
    bool terminal = (next_m.white_pieces() == 0);
    if (terminal) {
      std::cout << "Move " << i << ": TERMINAL CAPTURE (WIN for us)" << std::endl;
      found_loss = true;  // terminal capture = opponent loses
      all_wins = false;
      continue;
    }

    std::cout << "Move " << i << " -> material " << next_m << ": ";
    switch (succ_val) {
      case Value::WIN: std::cout << "opponent WIN (bad)"; break;
      case Value::LOSS: std::cout << "opponent LOSS (good!)"; found_loss = true; all_wins = false; break;
      case Value::DRAW: std::cout << "opponent DRAW"; all_wins = false; break;
      default: std::cout << "UNKNOWN"; all_wins = false; break;
    }
    std::cout << std::endl;
  }

  std::cout << "\n=== Analysis ===" << std::endl;
  std::cout << "Found LOSS for opponent: " << (found_loss ? "YES" : "NO") << std::endl;
  std::cout << "All successors WIN for opponent: " << (all_wins ? "YES" : "NO") << std::endl;

  if (found_loss) {
    std::cout << "Position should be WIN (has move to opponent LOSS)" << std::endl;
  } else if (all_wins) {
    std::cout << "Position should be LOSS (all moves lead to opponent WIN)" << std::endl;
  } else {
    std::cout << "Position should be DRAW (no WIN move, not all LOSS)" << std::endl;
  }

  return 0;
}
