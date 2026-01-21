#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <unordered_map>

// Cache for looking up successors
std::unordered_map<Material, std::vector<Value>> wdl_cache;
std::unordered_map<Material, std::vector<DTM>> dtm_cache;

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

DTM lookup_dtm(const Board& b) {
  Material m = get_material(b);
  if (dtm_cache.find(m) == dtm_cache.end()) {
    dtm_cache[m] = load_dtm(m);
  }
  auto& tb = dtm_cache[m];
  if (tb.empty()) return DTM_UNKNOWN;
  std::size_t idx = board_to_index(b, m);
  if (idx >= tb.size()) return DTM_UNKNOWN;
  return tb[idx];
}

int main() {
  // Investigate DTM error at index 70122 of material 001111
  // White: 0x8080000, Black: 0x8001, Kings: 0x80001
  // WDL: WIN, Expected DTM: 4, Got DTM: 6

  Board b;
  b.white = 0x8080000;
  b.black = 0x8001;
  b.kings = 0x80001;
  b.n_reversible = 0;

  std::cout << "Board:\n" << b << std::endl;

  Material m = get_material(b);
  std::cout << "Material: " << m << std::endl;

  std::size_t idx = board_to_index(b, m);
  std::cout << "Index: " << idx << std::endl;

  // Load WDL and DTM
  Value wdl_val = lookup_wdl(b);
  DTM dtm_val = lookup_dtm(b);

  std::cout << "WDL: ";
  switch (wdl_val) {
    case Value::WIN: std::cout << "WIN"; break;
    case Value::LOSS: std::cout << "LOSS"; break;
    case Value::DRAW: std::cout << "DRAW"; break;
    default: std::cout << "UNKNOWN"; break;
  }
  std::cout << std::endl;
  std::cout << "DTM: " << dtm_val << std::endl;

  // Generate moves
  std::vector<Move> moves;
  generateMoves(b, moves);
  std::cout << "\nMoves available: " << moves.size() << std::endl;

  // Analyze all successors with DTM
  std::cout << "\n=== All successor positions ===" << std::endl;

  DTM best_opp_loss = DTM_UNKNOWN;

  for (size_t i = 0; i < moves.size(); i++) {
    Board next = makeMove(b, moves[i]);
    Material next_m = get_material(next);
    Value succ_wdl = lookup_wdl(next);
    DTM succ_dtm = lookup_dtm(next);

    // Check for terminal capture
    bool terminal = (next_m.white_pieces() == 0);
    if (terminal) {
      succ_wdl = Value::LOSS;
      succ_dtm = DTM_LOSS_TERMINAL;
    }

    std::cout << "Move " << i << " -> " << next_m << ": ";
    switch (succ_wdl) {
      case Value::WIN: std::cout << "WIN"; break;
      case Value::LOSS: std::cout << "LOSS"; break;
      case Value::DRAW: std::cout << "DRAW"; break;
      default: std::cout << "UNKNOWN"; break;
    }
    std::cout << " DTM=" << succ_dtm << std::endl;

    if (succ_wdl == Value::LOSS) {
      if (succ_dtm != DTM_UNKNOWN && (best_opp_loss == DTM_UNKNOWN || succ_dtm > best_opp_loss)) {
        best_opp_loss = succ_dtm;
      }
    }
  }

  std::cout << "\n=== DTM Analysis ===" << std::endl;
  std::cout << "best_opp_loss = " << best_opp_loss << std::endl;

  if (wdl_val == Value::WIN && best_opp_loss != DTM_UNKNOWN) {
    int opp_moves = (best_opp_loss == DTM_LOSS_TERMINAL) ? 0 : -best_opp_loss;
    DTM expected_dtm = opp_moves + 1;  // dtm_win encoding
    std::cout << "opp_moves = " << opp_moves << std::endl;
    std::cout << "Expected DTM (opp_moves + 1) = " << expected_dtm << std::endl;
    std::cout << "Actual DTM = " << dtm_val << std::endl;
    if (expected_dtm != dtm_val) {
      std::cout << "MISMATCH!" << std::endl;
    }
  }

  return 0;
}
