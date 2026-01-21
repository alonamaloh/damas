#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <cstring>

namespace {

// Cache loaded tablebases
std::unordered_map<Material, std::vector<Value>> wdl_cache;
std::unordered_map<Material, std::vector<DTM>> dtm_cache;

// Get WDL value for a position (load if needed)
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

// Get DTM value for a position (load if needed)
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

// Convert Value to string for error reporting
const char* value_to_string(Value v) {
  switch (v) {
    case Value::UNKNOWN: return "UNKNOWN";
    case Value::WIN: return "WIN";
    case Value::LOSS: return "LOSS";
    case Value::DRAW: return "DRAW";
    default: return "?";
  }
}

// Print board for error reporting
void print_board(const Board& b) {
  std::cout << "    White: 0x" << std::hex << b.white << std::dec << std::endl;
  std::cout << "    Black: 0x" << std::hex << b.black << std::dec << std::endl;
  std::cout << "    Kings: 0x" << std::hex << b.kings << std::dec << std::endl;
}

// Verify a single position
// Returns true if position is consistent, false if error found
bool verify_position(const Board& board, Value expected_wdl, DTM expected_dtm,
                     const Material& /*m*/, std::size_t idx, bool verbose) {
  std::vector<Move> moves;
  generateMoves(board, moves);

  // Terminal position check (no legal moves)
  if (moves.empty()) {
    bool wdl_ok = (expected_wdl == Value::LOSS);
    bool dtm_ok = (expected_dtm == DTM_LOSS_TERMINAL);

    if (!wdl_ok || !dtm_ok) {
      if (verbose) {
        std::cout << "  ERROR at index " << idx << " (terminal position):" << std::endl;
        print_board(board);
        std::cout << "    Expected: WDL=" << value_to_string(Value::LOSS)
                  << " DTM=" << DTM_LOSS_TERMINAL << std::endl;
        std::cout << "    Got: WDL=" << value_to_string(expected_wdl)
                  << " DTM=" << expected_dtm << std::endl;
      }
      return false;
    }
    return true;
  }

  // Collect successor values
  bool found_loss = false;
  bool all_wins = true;
  DTM best_opp_loss = DTM_UNKNOWN;  // For WIN verification (highest LOSS DTM)
  DTM best_opp_win = DTM_UNKNOWN;   // For LOSS verification (highest WIN DTM)

  for (const Move& move : moves) {
    Board next = makeMove(board, move);
    Value succ_wdl = lookup_wdl(next);
    DTM succ_dtm = lookup_dtm(next);

    // Handle terminal captures (opponent has no pieces)
    Material next_m = get_material(next);
    if (next_m.white_pieces() == 0) {
      // After makeMove, board is flipped, so "white" having 0 pieces means
      // the side to move (opponent from our perspective) has lost
      succ_wdl = Value::LOSS;
      succ_dtm = DTM_LOSS_TERMINAL;
    }

    if (succ_wdl == Value::LOSS) {
      found_loss = true;
      // Track quickest loss for opponent (we want fastest win)
      // DTM_LOSS_TERMINAL (-128) = 0 moves, is best
      // Otherwise, highest (least negative) = fewest moves
      if (succ_dtm != DTM_UNKNOWN) {
        if (best_opp_loss == DTM_UNKNOWN) {
          best_opp_loss = succ_dtm;
        } else if (succ_dtm == DTM_LOSS_TERMINAL) {
          best_opp_loss = DTM_LOSS_TERMINAL;  // Terminal is always fastest
        } else if (best_opp_loss != DTM_LOSS_TERMINAL && succ_dtm > best_opp_loss) {
          best_opp_loss = succ_dtm;
        }
      }
    }
    if (succ_wdl != Value::WIN) {
      all_wins = false;
    }
    if (succ_wdl == Value::WIN && succ_dtm > 0) {
      // Track highest (largest) WIN DTM for LOSS verification
      if (best_opp_win == DTM_UNKNOWN || succ_dtm > best_opp_win) {
        best_opp_win = succ_dtm;
      }
    }
  }

  // Verify WDL
  Value computed_wdl;
  if (found_loss) {
    computed_wdl = Value::WIN;
  } else if (all_wins) {
    computed_wdl = Value::LOSS;
  } else {
    computed_wdl = Value::DRAW;
  }

  if (computed_wdl != expected_wdl) {
    if (verbose) {
      std::cout << "  WDL ERROR at index " << idx << ":" << std::endl;
      print_board(board);
      std::cout << "    Expected WDL: " << value_to_string(expected_wdl) << std::endl;
      std::cout << "    Computed WDL: " << value_to_string(computed_wdl) << std::endl;
      std::cout << "    found_loss=" << found_loss << " all_wins=" << all_wins << std::endl;
      std::cout << "    Successor values:" << std::endl;
      for (const Move& move : moves) {
        Board next = makeMove(board, move);
        Material nm = get_material(next);
        Value sv = lookup_wdl(next);
        DTM sd = lookup_dtm(next);
        // Apply terminal check for display
        if (nm.white_pieces() == 0) {
          sv = Value::LOSS;
          sd = DTM_LOSS_TERMINAL;
        }
        std::cout << "      -> " << value_to_string(sv) << " DTM=" << sd
                  << " (mat=" << nm << " wp=" << nm.white_pieces() << ")" << std::endl;
      }
    }
    return false;
  }

  // Verify DTM
  DTM expected_computed_dtm;
  if (expected_wdl == Value::DRAW) {
    expected_computed_dtm = DTM_DRAW;
  } else if (expected_wdl == Value::WIN) {
    // WIN in (opponent's quickest loss moves + 1) moves
    // Opponent's quickest loss = highest negative DTM (least negative = fewest moves)
    if (best_opp_loss == DTM_UNKNOWN) {
      // Should not happen if WDL is correct
      if (verbose) {
        std::cout << "  DTM ERROR at index " << idx << ": WIN but no LOSS successor found" << std::endl;
      }
      return false;
    }
    int opp_moves = (best_opp_loss == DTM_LOSS_TERMINAL) ? 0 : -best_opp_loss;
    expected_computed_dtm = dtm_win(opp_moves + 1);
  } else {  // LOSS
    // LOSS in (opponent's slowest win) moves
    // Opponent's slowest win = highest positive DTM
    if (best_opp_win == DTM_UNKNOWN) {
      // Should not happen if WDL is correct (all successors are WIN)
      if (verbose) {
        std::cout << "  DTM ERROR at index " << idx << ": LOSS but no WIN successor found" << std::endl;
      }
      return false;
    }
    expected_computed_dtm = dtm_loss(best_opp_win);
  }

  if (expected_dtm != expected_computed_dtm) {
    if (verbose) {
      std::cout << "  DTM ERROR at index " << idx << ":" << std::endl;
      print_board(board);
      std::cout << "    WDL: " << value_to_string(expected_wdl) << std::endl;
      std::cout << "    Expected DTM: " << expected_computed_dtm << std::endl;
      std::cout << "    Got DTM: " << expected_dtm << std::endl;
      if (expected_wdl == Value::WIN) {
        std::cout << "    best_opp_loss=" << best_opp_loss << std::endl;
      } else {
        std::cout << "    best_opp_win=" << best_opp_win << std::endl;
      }
      std::cout << "    Successor values:" << std::endl;
      for (const Move& move : moves) {
        Board next = makeMove(board, move);
        Value sv = lookup_wdl(next);
        DTM sd = lookup_dtm(next);
        std::cout << "      -> " << value_to_string(sv) << " DTM=" << sd << std::endl;
      }
    }
    return false;
  }

  return true;
}

// Generate all material configurations with up to n total pieces
std::vector<Material> all_materials(int max_pieces) {
  std::vector<Material> result;

  for (int total = 2; total <= max_pieces; ++total) {
    for (int bwp = 0; bwp <= std::min(4, total - 1); ++bwp) {
      for (int bbp = 0; bbp <= std::min(4, total - bwp - 1); ++bbp) {
        for (int owp = 0; owp <= std::min(24, total - bwp - bbp - 1); ++owp) {
          for (int obp = 0; obp <= std::min(24 - owp, total - bwp - bbp - owp - 1); ++obp) {
            int remaining = total - bwp - bbp - owp - obp;
            for (int wq = 0; wq <= remaining; ++wq) {
              int bq = remaining - wq;
              if (bq >= 0) {
                Material m{bwp, bbp, owp, obp, wq, bq};
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

void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " [options] <max_pieces>" << std::endl;
  std::cerr << std::endl;
  std::cerr << "Options:" << std::endl;
  std::cerr << "  -v, --verbose    Show detailed error information" << std::endl;
  std::cerr << "  --wdl-only       Only verify WDL (skip DTM verification)" << std::endl;
  std::cerr << std::endl;
  std::cerr << "Verifies all tablebases up to max_pieces (2-8)." << std::endl;
  std::cerr << "Checks that each position's value is consistent with its successors." << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
  bool verbose = false;
  bool wdl_only = false;
  int max_pieces = 0;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
    } else if (std::strcmp(argv[i], "--wdl-only") == 0) {
      wdl_only = true;
    } else if (argv[i][0] != '-') {
      max_pieces = std::atoi(argv[i]);
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  if (max_pieces < 2 || max_pieces > 8) {
    print_usage(argv[0]);
    return 1;
  }

  std::cout << "=== Tablebase Verification ===" << std::endl;
  std::cout << "Verifying up to " << max_pieces << " pieces" << std::endl;
  if (wdl_only) {
    std::cout << "Mode: WDL only" << std::endl;
  }
  std::cout << std::endl;

  std::vector<Material> materials = all_materials(max_pieces);

  std::size_t total_materials = 0;
  std::size_t total_positions = 0;
  std::size_t total_wdl_errors = 0;
  std::size_t total_dtm_errors = 0;
  std::size_t skipped_materials = 0;

  for (const Material& m : materials) {
    // Load WDL tablebase
    std::vector<Value> wdl = load_tablebase(m);
    if (wdl.empty()) {
      skipped_materials++;
      continue;
    }
    wdl_cache[m] = wdl;

    // Load DTM tablebase (if not wdl_only)
    std::vector<DTM> dtm;
    if (!wdl_only) {
      dtm = load_dtm(m);
      if (dtm.empty()) {
        std::cout << "Skipping material " << m << " (DTM not found)" << std::endl;
        skipped_materials++;
        continue;
      }
      dtm_cache[m] = dtm;
    }

    std::size_t size = material_size(m);
    std::cout << "Verifying material " << m << " (" << size << " positions)..." << std::endl;

    std::size_t wdl_errors = 0;
    std::size_t dtm_errors = 0;

    for (std::size_t idx = 0; idx < size; ++idx) {
      Board board = index_to_board(idx, m);
      Value expected_wdl = wdl[idx];
      DTM expected_dtm = wdl_only ? DTM_DRAW : dtm[idx];

      if (wdl_only) {
        // Only verify WDL consistency
        std::vector<Move> moves;
        generateMoves(board, moves);

        Value computed_wdl;
        if (moves.empty()) {
          computed_wdl = Value::LOSS;
        } else {
          bool found_loss = false;
          bool all_wins = true;
          for (const Move& move : moves) {
            Board next = makeMove(board, move);
            Value succ_wdl = lookup_wdl(next);
            Material next_m = get_material(next);
            if (next_m.white_pieces() == 0) {
              succ_wdl = Value::LOSS;
            }
            if (succ_wdl == Value::LOSS) found_loss = true;
            if (succ_wdl != Value::WIN) all_wins = false;
          }
          if (found_loss) computed_wdl = Value::WIN;
          else if (all_wins) computed_wdl = Value::LOSS;
          else computed_wdl = Value::DRAW;
        }

        if (computed_wdl != expected_wdl) {
          wdl_errors++;
          if (verbose) {
            std::cout << "  WDL ERROR at index " << idx << ":" << std::endl;
            print_board(board);
            std::cout << "    Expected: " << value_to_string(expected_wdl) << std::endl;
            std::cout << "    Computed: " << value_to_string(computed_wdl) << std::endl;
          }
        }
      } else {
        // Full WDL + DTM verification
        if (!verify_position(board, expected_wdl, expected_dtm, m, idx, verbose)) {
          // Check which type of error
          std::vector<Move> moves;
          generateMoves(board, moves);

          Value computed_wdl;
          if (moves.empty()) {
            computed_wdl = Value::LOSS;
          } else {
            bool found_loss = false;
            bool all_wins = true;
            for (const Move& move : moves) {
              Board next = makeMove(board, move);
              Value succ_wdl = lookup_wdl(next);
              Material next_m = get_material(next);
              if (next_m.white_pieces() == 0) succ_wdl = Value::LOSS;
              if (succ_wdl == Value::LOSS) found_loss = true;
              if (succ_wdl != Value::WIN) all_wins = false;
            }
            if (found_loss) computed_wdl = Value::WIN;
            else if (all_wins) computed_wdl = Value::LOSS;
            else computed_wdl = Value::DRAW;
          }

          if (computed_wdl != expected_wdl) {
            wdl_errors++;
          } else {
            dtm_errors++;
          }
        }
      }
    }

    // Report per-material results
    if (wdl_errors == 0 && dtm_errors == 0) {
      std::cout << "  WDL: OK";
      if (!wdl_only) std::cout << "  DTM: OK";
      std::cout << std::endl;
    } else {
      if (wdl_errors > 0) {
        std::cout << "  WDL: " << wdl_errors << " ERRORS" << std::endl;
      } else {
        std::cout << "  WDL: OK" << std::endl;
      }
      if (!wdl_only) {
        if (dtm_errors > 0) {
          std::cout << "  DTM: " << dtm_errors << " ERRORS" << std::endl;
        } else {
          std::cout << "  DTM: OK" << std::endl;
        }
      }
    }

    total_materials++;
    total_positions += size;
    total_wdl_errors += wdl_errors;
    total_dtm_errors += dtm_errors;
  }

  std::cout << std::endl;
  std::cout << "=== Verification Summary ===" << std::endl;
  std::cout << "Materials verified: " << total_materials << std::endl;
  std::cout << "Materials skipped: " << skipped_materials << std::endl;
  std::cout << "Positions checked: " << total_positions << std::endl;
  std::cout << "WDL errors: " << total_wdl_errors << std::endl;
  if (!wdl_only) {
    std::cout << "DTM errors: " << total_dtm_errors << std::endl;
  }

  if (total_wdl_errors == 0 && total_dtm_errors == 0) {
    std::cout << std::endl << "All tablebases verified successfully!" << std::endl;
    return 0;
  } else {
    std::cout << std::endl << "VERIFICATION FAILED" << std::endl;
    return 1;
  }
}
