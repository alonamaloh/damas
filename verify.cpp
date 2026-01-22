// Tablebase verification tool
//
// Verifies that each position's stored value equals the minimax of its successors.
// Supports both uncompressed (wdl_*.bin) and compressed (cwdl_*.bin) tablebases.
// DTM verification is only available for uncompressed tablebases.

#include "tablebase.h"
#include "compression.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>

namespace {

// Score encoding for negamax (same as compression.cpp)
constexpr int SCORE_WIN = 1;
constexpr int SCORE_DRAW = 0;
constexpr int SCORE_LOSS = -1;

inline int value_to_score(Value v) {
  switch (v) {
    case Value::WIN: return SCORE_WIN;
    case Value::DRAW: return SCORE_DRAW;
    case Value::LOSS: return SCORE_LOSS;
    default: return SCORE_DRAW;
  }
}

inline Value score_to_value(int score) {
  if (score > 0) return Value::WIN;
  if (score < 0) return Value::LOSS;
  return Value::DRAW;
}

// Negamax to compute WDL from successors
// The lookup function returns a score for quiet positions
template<typename Lookup>
int compute_wdl_negamax(const Board& b, int alpha, int beta, Lookup&& lookup) {
  std::vector<Move> moves;
  generateMoves(b, moves);

  if (moves.empty()) {
    return SCORE_LOSS;
  }

  for (const Move& move : moves) {
    Board next = makeMove(b, move);

    // Terminal: captured all opponent pieces
    if (next.white == 0) {
      return SCORE_WIN;
    }

    int score = -compute_wdl_negamax(next, -beta, -alpha, lookup);

    if (score >= beta) {
      return score;
    }
    alpha = std::max(alpha, score);
  }

  return alpha;
}

// Compute WDL using negamax
template<typename Lookup>
Value compute_wdl(const Board& b, Lookup&& lookup) {
  // For quiet positions, the lookup gives us the stored value
  // For positions with captures, we compute via negamax
  if (!has_captures(b)) {
    return score_to_value(lookup(b));
  }
  int score = compute_wdl_negamax(b, SCORE_LOSS, SCORE_WIN, lookup);
  return score_to_value(score);
}

// ---------------------------------------------------------------------------
// Uncompressed tablebase support
// ---------------------------------------------------------------------------

struct UncompressedTables {
  std::unordered_map<Material, std::vector<Value>> wdl;
  std::unordered_map<Material, std::vector<DTM>> dtm;

  int lookup_score(const Board& b) {
    Material m = get_material(b);
    if (m.white_pieces() == 0) return SCORE_LOSS;

    auto it = wdl.find(m);
    if (it == wdl.end()) {
      wdl[m] = load_tablebase(m);
      it = wdl.find(m);
    }
    if (it->second.empty()) return SCORE_DRAW;

    std::size_t idx = board_to_index(b, m);
    return value_to_score(it->second[idx]);
  }

  Value lookup_wdl(const Board& b) {
    return score_to_value(lookup_score(b));
  }

  DTM lookup_dtm(const Board& b) {
    Material m = get_material(b);
    if (m.white_pieces() == 0) return DTM_LOSS_TERMINAL;

    auto it = dtm.find(m);
    if (it == dtm.end()) {
      dtm[m] = load_dtm(m);
      it = dtm.find(m);
    }
    if (it->second.empty()) return DTM_UNKNOWN;

    std::size_t idx = board_to_index(b, m);
    return it->second[idx];
  }
};

// ---------------------------------------------------------------------------
// DTM computation (uncompressed only)
// ---------------------------------------------------------------------------

DTM compute_dtm(const Board& board, Value wdl, UncompressedTables& tables) {
  if (wdl == Value::DRAW) return DTM_DRAW;

  std::vector<Move> moves;
  generateMoves(board, moves);

  if (moves.empty()) return DTM_LOSS_TERMINAL;

  if (wdl == Value::WIN) {
    // Find quickest win: minimize opponent's loss depth
    DTM best = DTM_UNKNOWN;
    for (const Move& move : moves) {
      Board next = makeMove(board, move);
      if (tables.lookup_wdl(next) == Value::LOSS) {
        DTM d = tables.lookup_dtm(next);
        if (d == DTM_LOSS_TERMINAL) return dtm_win(1);
        if (best == DTM_UNKNOWN || d > best) best = d;
      }
    }
    return best == DTM_UNKNOWN ? DTM_UNKNOWN : dtm_win(1 - best);
  }

  // LOSS: opponent's slowest win
  DTM worst = DTM_UNKNOWN;
  for (const Move& move : moves) {
    DTM d = tables.lookup_dtm(makeMove(board, move));
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
// Error reporting
// ---------------------------------------------------------------------------

const char* value_name(Value v) {
  switch (v) {
    case Value::WIN:  return "WIN";
    case Value::LOSS: return "LOSS";
    case Value::DRAW: return "DRAW";
    default:          return "UNKNOWN";
  }
}

void print_board(const Board& b) {
  std::cout << "    White: 0x" << std::hex << b.white << std::dec << "\n";
  std::cout << "    Black: 0x" << std::hex << b.black << std::dec << "\n";
  std::cout << "    Kings: 0x" << std::hex << b.kings << std::dec << "\n";
}

template<typename Lookup>
void show_wdl_error(const Board& b, std::size_t idx,
                    Value stored, Value computed, Lookup&& lookup) {
  std::cout << "  WDL ERROR at index " << idx << ":\n";
  print_board(b);
  std::cout << "    Stored: " << value_name(stored)
            << "  Computed: " << value_name(computed) << "\n";
  std::cout << "    Successors:\n";

  std::vector<Move> moves;
  generateMoves(b, moves);
  for (const Move& move : moves) {
    Board next = makeMove(b, move);
    Material nm = get_material(next);
    Value sv = (nm.white_pieces() == 0) ? Value::LOSS : score_to_value(lookup(next));
    std::cout << "      -> " << value_name(sv) << " (mat=" << nm << ")\n";
  }
}

void show_dtm_error(const Board& b, std::size_t idx,
                    DTM stored, DTM computed, UncompressedTables& tables) {
  std::cout << "  DTM ERROR at index " << idx << ":\n";
  print_board(b);
  std::cout << "    Stored: " << stored << "  Computed: " << computed << "\n";
  std::cout << "    Successors:\n";

  std::vector<Move> moves;
  generateMoves(b, moves);
  for (const Move& move : moves) {
    Board next = makeMove(b, move);
    std::cout << "      -> WDL=" << value_name(tables.lookup_wdl(next))
              << " DTM=" << tables.lookup_dtm(next) << "\n";
  }
}

// ---------------------------------------------------------------------------
// Verification core
// ---------------------------------------------------------------------------

struct VerifyStats {
  std::size_t wdl_errors = 0;
  std::size_t dtm_errors = 0;
};

template<typename Lookup>
VerifyStats verify_wdl(const Material& m, std::size_t size,
                       Lookup&& lookup,
                       std::function<Value(std::size_t)> get_stored,
                       bool verbose) {
  VerifyStats stats;

  for (std::size_t idx = 0; idx < size; ++idx) {
    Board board = index_to_board(idx, m);
    Value stored = get_stored(idx);
    Value computed = compute_wdl(board, lookup);

    if (stored != computed) {
      stats.wdl_errors++;
      if (verbose) {
        show_wdl_error(board, idx, stored, computed, lookup);
      }
    }
  }

  return stats;
}

// ---------------------------------------------------------------------------
// Command-line interface
// ---------------------------------------------------------------------------

struct Options {
  std::string directory = ".";
  int max_pieces = 0;
  bool verbose = false;
  bool wdl_only = false;
  bool compressed = false;
  bool auto_detect = true;
};

Options parse_args(int argc, char* argv[]) {
  Options opts;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
      opts.verbose = true;
    } else if (std::strcmp(argv[i], "--wdl-only") == 0) {
      opts.wdl_only = true;
    } else if (std::strcmp(argv[i], "--compressed") == 0) {
      opts.compressed = true;
      opts.auto_detect = false;
    } else if (std::strcmp(argv[i], "--uncompressed") == 0) {
      opts.compressed = false;
      opts.auto_detect = false;
    } else if (argv[i][0] != '-') {
      positional.push_back(argv[i]);
    }
  }

  if (positional.size() == 1) {
    opts.max_pieces = std::atoi(positional[0].c_str());
  } else if (positional.size() == 2) {
    opts.directory = positional[0];
    opts.max_pieces = std::atoi(positional[1].c_str());
  }

  return opts;
}

void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options] [directory] <max_pieces>\n\n"
            << "Options:\n"
            << "  -v, --verbose      Show detailed error information\n"
            << "  --wdl-only         Only verify WDL (skip DTM)\n"
            << "  --compressed       Use compressed tablebases (cwdl_*.bin)\n"
            << "  --uncompressed     Use uncompressed tablebases (wdl_*.bin)\n\n"
            << "If neither --compressed nor --uncompressed is specified,\n"
            << "auto-detects based on which files are present.\n\n"
            << "DTM verification is only available for uncompressed tablebases.\n";
}

} // namespace

int main(int argc, char* argv[]) {
  Options opts = parse_args(argc, argv);

  if (opts.max_pieces < 2 || opts.max_pieces > 8) {
    usage(argv[0]);
    return 1;
  }

  // Auto-detect compressed vs uncompressed
  if (opts.auto_detect) {
    // Check for cwdl files first (compressed)
    for (const auto& entry : std::filesystem::directory_iterator(opts.directory)) {
      if (entry.path().filename().string().find("cwdl_") == 0) {
        opts.compressed = true;
        break;
      }
    }
  }

  std::cout << "=== Tablebase Verification ===\n"
            << "Directory: " << opts.directory << "\n"
            << "Mode: " << (opts.compressed ? "compressed" : "uncompressed") << "\n"
            << "Verifying up to " << opts.max_pieces << " pieces\n";
  if (opts.wdl_only || opts.compressed) {
    std::cout << "WDL only" << (opts.compressed ? " (DTM not available for compressed)" : "") << "\n";
  }
  std::cout << "\n";

  std::vector<Material> materials = all_materials(opts.max_pieces);

  std::size_t total_materials = 0;
  std::size_t total_positions = 0;
  std::size_t total_wdl_errors = 0;
  std::size_t total_dtm_errors = 0;
  std::size_t skipped = 0;

  if (opts.compressed) {
    // Compressed verification
    CompressedTablebaseManager manager(opts.directory);

    for (const Material& m : materials) {
      const CompressedTablebase* tb = manager.get_tablebase(m);
      if (!tb) {
        skipped++;
        continue;
      }

      std::size_t size = tb->num_positions;
      std::cout << "Verifying " << m << " (" << size << " positions)...";
      std::cout.flush();

      auto lookup = [&manager](const Board& pos) -> int {
        Material pm = get_material(pos);
        if (pm.white_pieces() == 0) return SCORE_LOSS;
        const CompressedTablebase* ptb = manager.get_tablebase(pm);
        if (!ptb) return SCORE_DRAW;
        return value_to_score(lookup_compressed(*ptb, board_to_index(pos, pm)));
      };

      auto get_stored = [&tb, &m](std::size_t idx) -> Value {
        return lookup_compressed(*tb, idx);
      };

      VerifyStats stats = verify_wdl(m, size, lookup, get_stored, opts.verbose);

      std::cout << " WDL: " << (stats.wdl_errors == 0 ? "OK" : std::to_string(stats.wdl_errors) + " ERRORS")
                << "\n";

      total_materials++;
      total_positions += size;
      total_wdl_errors += stats.wdl_errors;
    }
  } else {
    // Uncompressed verification
    UncompressedTables tables;

    for (const Material& m : materials) {
      std::vector<Value> wdl = load_tablebase(m);
      if (wdl.empty()) {
        skipped++;
        continue;
      }
      tables.wdl[m] = wdl;

      bool check_dtm = !opts.wdl_only;
      std::vector<DTM> dtm;
      if (check_dtm) {
        dtm = load_dtm(m);
        if (dtm.empty()) {
          check_dtm = false;
        } else {
          tables.dtm[m] = dtm;
        }
      }

      std::size_t size = material_size(m);
      std::cout << "Verifying " << m << " (" << size << " positions)...";
      std::cout.flush();

      auto lookup = [&tables](const Board& pos) -> int {
        return tables.lookup_score(pos);
      };

      auto get_stored = [&wdl](std::size_t idx) -> Value {
        return wdl[idx];
      };

      VerifyStats stats = verify_wdl(m, size, lookup, get_stored, opts.verbose);

      // DTM verification
      if (check_dtm) {
        for (std::size_t idx = 0; idx < size; ++idx) {
          Board board = index_to_board(idx, m);
          DTM stored_dtm = dtm[idx];
          DTM computed_dtm = compute_dtm(board, wdl[idx], tables);

          if (stored_dtm != computed_dtm) {
            stats.dtm_errors++;
            if (opts.verbose) {
              show_dtm_error(board, idx, stored_dtm, computed_dtm, tables);
            }
          }
        }
      }

      std::cout << " WDL: " << (stats.wdl_errors == 0 ? "OK" : std::to_string(stats.wdl_errors) + " ERRORS");
      if (check_dtm) {
        std::cout << "  DTM: " << (stats.dtm_errors == 0 ? "OK" : std::to_string(stats.dtm_errors) + " ERRORS");
      }
      std::cout << "\n";

      total_materials++;
      total_positions += size;
      total_wdl_errors += stats.wdl_errors;
      total_dtm_errors += stats.dtm_errors;
    }
  }

  std::cout << "\n=== Summary ===\n"
            << "Materials: " << total_materials << " verified, " << skipped << " skipped\n"
            << "Positions: " << total_positions << "\n"
            << "WDL errors: " << total_wdl_errors << "\n";
  if (!opts.compressed && !opts.wdl_only) {
    std::cout << "DTM errors: " << total_dtm_errors << "\n";
  }

  bool success = (total_wdl_errors == 0 && total_dtm_errors == 0);
  std::cout << "\n" << (success ? "All tablebases verified." : "VERIFICATION FAILED") << "\n";
  return success ? 0 : 1;
}
