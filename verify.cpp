// Tablebase verification tool
//
// Verifies that each position's stored value equals the minimax of its successors.
// For WDL: WIN if any successor is LOSS, LOSS if all successors WIN, else DRAW.
// For DTM: WIN in N = 1 + min(opponent's loss depth), LOSS in N = max(opponent's win depth).

#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Tablebase cache
// ---------------------------------------------------------------------------

std::unordered_map<Material, std::vector<Value>> wdl_cache;
std::unordered_map<Material, std::vector<DTM>> dtm_cache;

Value get_wdl(const Board& b) {
  Material m = get_material(b);

  // Terminal capture: opponent has no pieces
  if (m.white_pieces() == 0) return Value::LOSS;

  auto it = wdl_cache.find(m);
  if (it == wdl_cache.end()) {
    wdl_cache[m] = load_tablebase(m);
    it = wdl_cache.find(m);
  }

  if (it->second.empty()) return Value::UNKNOWN;
  std::size_t idx = board_to_index(b, m);
  return idx < it->second.size() ? it->second[idx] : Value::UNKNOWN;
}

DTM get_dtm(const Board& b) {
  Material m = get_material(b);

  // Terminal capture
  if (m.white_pieces() == 0) return DTM_LOSS_TERMINAL;

  auto it = dtm_cache.find(m);
  if (it == dtm_cache.end()) {
    dtm_cache[m] = load_dtm(m);
    it = dtm_cache.find(m);
  }

  if (it->second.empty()) return DTM_UNKNOWN;
  std::size_t idx = board_to_index(b, m);
  return idx < it->second.size() ? it->second[idx] : DTM_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Minimax computation
// ---------------------------------------------------------------------------

Value compute_wdl(const Board& board) {
  std::vector<Move> moves;
  generateMoves(board, moves);

  if (moves.empty()) return Value::LOSS;

  bool dominated = true;  // All successors are WIN (opponent wins all)

  for (const Move& move : moves) {
    Value v = get_wdl(makeMove(board, move));
    if (v == Value::LOSS) return Value::WIN;  // Found winning move
    if (v != Value::WIN) dominated = false;
  }

  return dominated ? Value::LOSS : Value::DRAW;
}

DTM compute_dtm(const Board& board, Value wdl) {
  if (wdl == Value::DRAW) return DTM_DRAW;

  std::vector<Move> moves;
  generateMoves(board, moves);

  if (moves.empty()) return DTM_LOSS_TERMINAL;

  if (wdl == Value::WIN) {
    // Find quickest win: minimize opponent's loss depth
    DTM best = DTM_UNKNOWN;
    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      if (get_wdl(next) == Value::LOSS) {
        DTM d = get_dtm(next);
        if (d == DTM_LOSS_TERMINAL) return dtm_win(1);  // Immediate capture
        if (best == DTM_UNKNOWN || d > best) best = d;  // Less negative = faster
      }
    }
    return best == DTM_UNKNOWN ? DTM_UNKNOWN : dtm_win(1 - best);
  }

  // LOSS: opponent's slowest win (maximize opponent's win depth)
  DTM worst = DTM_UNKNOWN;
  for (const Move& move : moves) {
    DTM d = get_dtm(makeMove(board, move));
    if (d > 0 && (worst == DTM_UNKNOWN || d > worst)) worst = d;
  }
  return worst == DTM_UNKNOWN ? DTM_UNKNOWN : dtm_loss(worst);
}

// ---------------------------------------------------------------------------
// Material enumeration
// ---------------------------------------------------------------------------

std::vector<Material> all_materials(int max_pieces) {
  std::vector<Material> result;

  for (int n = 2; n <= max_pieces; ++n) {
    // Enumerate all distributions of n pieces across 6 categories:
    // back_white_pawns, back_black_pawns, other_white_pawns, other_black_pawns,
    // white_queens, black_queens
    for (int bwp = 0; bwp <= std::min(4, n); ++bwp) {
      for (int bbp = 0; bbp <= std::min(4, n - bwp); ++bbp) {
        for (int owp = 0; owp <= std::min(24, n - bwp - bbp); ++owp) {
          for (int obp = 0; obp <= std::min(24 - owp, n - bwp - bbp - owp); ++obp) {
            int queens = n - bwp - bbp - owp - obp;
            for (int wq = 0; wq <= queens; ++wq) {
              Material m{bwp, bbp, owp, obp, wq, queens - wq};
              if (m.white_pieces() > 0 && m.black_pieces() > 0) {
                result.push_back(m);
              }
            }
          }
        }
      }
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

struct Errors {
  std::size_t wdl = 0;
  std::size_t dtm = 0;
};

const char* name(Value v) {
  switch (v) {
    case Value::WIN:  return "WIN";
    case Value::LOSS: return "LOSS";
    case Value::DRAW: return "DRAW";
    default:          return "UNKNOWN";
  }
}

void show_error(const Board& b, std::size_t idx,
                Value stored_wdl, Value computed_wdl,
                DTM stored_dtm, DTM computed_dtm) {
  std::cout << "  ERROR at index " << idx << ":\n";
  std::cout << "    White: 0x" << std::hex << b.white << std::dec << "\n";
  std::cout << "    Black: 0x" << std::hex << b.black << std::dec << "\n";
  std::cout << "    Kings: 0x" << std::hex << b.kings << std::dec << "\n";

  if (stored_wdl != computed_wdl) {
    std::cout << "    WDL: stored=" << name(stored_wdl)
              << " computed=" << name(computed_wdl) << "\n";
  }
  if (stored_dtm != computed_dtm) {
    std::cout << "    DTM: stored=" << stored_dtm
              << " computed=" << computed_dtm << "\n";
  }

  std::cout << "    Successors:\n";
  std::vector<Move> moves;
  generateMoves(b, moves);
  for (const Move& move : moves) {
    Board next = makeMove(b, move);
    std::cout << "      -> " << name(get_wdl(next))
              << " DTM=" << get_dtm(next) << "\n";
  }
}

Errors verify(const Material& m, bool check_dtm, bool verbose) {
  Errors errors;

  std::vector<Value> wdl = load_tablebase(m);
  if (wdl.empty()) return errors;
  wdl_cache[m] = wdl;

  std::vector<DTM> dtm;
  if (check_dtm) {
    dtm = load_dtm(m);
    if (dtm.empty()) return errors;
    dtm_cache[m] = dtm;
  }

  std::size_t size = material_size(m);

  for (std::size_t idx = 0; idx < size; ++idx) {
    Board board = index_to_board(idx, m);

    Value stored_wdl = wdl[idx];
    Value computed_wdl = compute_wdl(board);

    DTM stored_dtm = check_dtm ? dtm[idx] : DTM_DRAW;
    DTM computed_dtm = check_dtm ? compute_dtm(board, stored_wdl) : DTM_DRAW;

    bool wdl_ok = (stored_wdl == computed_wdl);
    bool dtm_ok = !check_dtm || (stored_dtm == computed_dtm);

    if (!wdl_ok) errors.wdl++;
    if (!dtm_ok) errors.dtm++;

    if (verbose && (!wdl_ok || !dtm_ok)) {
      show_error(board, idx, stored_wdl, computed_wdl, stored_dtm, computed_dtm);
    }
  }

  return errors;
}

// ---------------------------------------------------------------------------
// Command-line interface
// ---------------------------------------------------------------------------

struct Options {
  int max_pieces = 0;
  bool verbose = false;
  bool wdl_only = false;
};

Options parse_args(int argc, char* argv[]) {
  Options opts;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
      opts.verbose = true;
    } else if (std::strcmp(argv[i], "--wdl-only") == 0) {
      opts.wdl_only = true;
    } else if (argv[i][0] != '-') {
      opts.max_pieces = std::atoi(argv[i]);
    }
  }

  return opts;
}

void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options] <max_pieces>\n\n"
            << "Options:\n"
            << "  -v, --verbose    Show detailed error information\n"
            << "  --wdl-only       Only verify WDL (skip DTM verification)\n\n"
            << "Verifies all tablebases up to max_pieces (2-8).\n";
}

} // namespace

int main(int argc, char* argv[]) {
  Options opts = parse_args(argc, argv);

  if (opts.max_pieces < 2 || opts.max_pieces > 8) {
    usage(argv[0]);
    return 1;
  }

  std::cout << "=== Tablebase Verification ===\n"
            << "Verifying up to " << opts.max_pieces << " pieces\n";
  if (opts.wdl_only) std::cout << "Mode: WDL only\n";
  std::cout << "\n";

  std::vector<Material> materials = all_materials(opts.max_pieces);

  std::size_t total_materials = 0;
  std::size_t total_positions = 0;
  std::size_t total_wdl_errors = 0;
  std::size_t total_dtm_errors = 0;
  std::size_t skipped = 0;

  for (const Material& m : materials) {
    std::vector<Value> wdl = load_tablebase(m);
    if (wdl.empty()) {
      skipped++;
      continue;
    }

    if (!opts.wdl_only && load_dtm(m).empty()) {
      std::cout << "Skipping " << m << " (no DTM)\n";
      skipped++;
      continue;
    }

    std::size_t size = material_size(m);
    std::cout << "Verifying " << m << " (" << size << " positions)...";
    std::cout.flush();

    Errors errors = verify(m, !opts.wdl_only, opts.verbose);

    std::cout << " WDL: " << (errors.wdl == 0 ? "OK" : std::to_string(errors.wdl) + " ERRORS");
    if (!opts.wdl_only) {
      std::cout << "  DTM: " << (errors.dtm == 0 ? "OK" : std::to_string(errors.dtm) + " ERRORS");
    }
    std::cout << "\n";

    total_materials++;
    total_positions += size;
    total_wdl_errors += errors.wdl;
    total_dtm_errors += errors.dtm;
  }

  std::cout << "\n=== Summary ===\n"
            << "Materials: " << total_materials << " verified, " << skipped << " skipped\n"
            << "Positions: " << total_positions << "\n"
            << "WDL errors: " << total_wdl_errors << "\n";
  if (!opts.wdl_only) {
    std::cout << "DTM errors: " << total_dtm_errors << "\n";
  }

  bool success = (total_wdl_errors == 0 && total_dtm_errors == 0);
  std::cout << "\n" << (success ? "All tablebases verified." : "VERIFICATION FAILED") << "\n";
  return success ? 0 : 1;
}
