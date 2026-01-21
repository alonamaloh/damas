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
  // Lookup: WQ:1,3 WP:2 BQ:4 BP:9 white to move
  // Human squares 1-32 -> code squares 0-31
  // WQ on 1,3 -> code 0,2; WP on 2 -> code 1; BQ on 4 -> code 3; BP on 9 -> code 8

  Board b;
  b.white = (1u << 0) | (1u << 1) | (1u << 2);  // squares 1,2,3 -> 0,1,2
  b.black = (1u << 3) | (1u << 8);               // squares 4,9 -> 3,8
  b.kings = (1u << 0) | (1u << 2) | (1u << 3);   // queens at 1,3,4 -> 0,2,3
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
