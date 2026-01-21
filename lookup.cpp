#include "tablebase.h"
#include "board.h"
#include <iostream>

int main() {
  // White queens on 1, 3 (0-indexed: 0, 2)
  // White pawn on 2 (0-indexed: 1)
  // Black pawn on 9 (0-indexed: 8)
  // Black queen on 31 (0-indexed: 30)
  Board b;
  b.white = (1u << 0) | (1u << 1) | (1u << 2);  // squares 0, 1, 2
  b.black = (1u << 8) | (1u << 30);  // squares 8, 30
  b.kings = (1u << 0) | (1u << 2) | (1u << 30);  // queens only
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
  } else {
    std::cout << "WDL value: ";
    switch (wdl[idx]) {
      case Value::WIN: std::cout << "WIN"; break;
      case Value::LOSS: std::cout << "LOSS"; break;
      case Value::DRAW: std::cout << "DRAW"; break;
      default: std::cout << "UNKNOWN"; break;
    }
    std::cout << std::endl;
  }

  // Load DTM
  std::vector<DTM> dtm_table = load_dtm(m);
  if (dtm_table.empty()) {
    std::cout << "DTM tablebase not found" << std::endl;
  } else {
    DTM d = dtm_table[idx];
    std::cout << "DTM: ";
    if (d == DTM_UNKNOWN) {
      std::cout << "UNKNOWN";
    } else if (d == DTM_DRAW) {
      std::cout << "DRAW (0)";
    } else if (d > 0) {
      int moves = dtm_to_moves(d);
      int plies = dtm_to_plies(d);
      std::cout << "WIN, mate in " << moves << " (" << plies << " plies, DTM=" << d << ")";
    } else if (d == DTM_LOSS_TERMINAL) {
      std::cout << "LOSS, terminal (DTM=" << d << ")";
    } else {
      int moves = dtm_to_moves(d);
      int plies = dtm_to_plies(d);
      std::cout << "LOSS, lost in " << moves << " (" << plies << " plies, DTM=" << d << ")";
    }
    std::cout << std::endl;
  }

  return 0;
}
