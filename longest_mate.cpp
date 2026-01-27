#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include "notation.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <unordered_map>

// Cache for DTM lookups
std::unordered_map<Material, std::vector<DTM>> dtm_cache;

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

// Generate all material configurations with up to n total pieces
std::vector<Material> all_materials(int max_pieces) {
  std::vector<Material> result;

  for (int total = 2; total <= max_pieces; ++total) {
    for (int bwp = 0; bwp <= std::min(4, total); ++bwp) {
      for (int bbp = 0; bbp <= std::min(4, total - bwp); ++bbp) {
        for (int owp = 0; owp <= std::min(24, total - bwp - bbp); ++owp) {
          for (int obp = 0; obp <= std::min(24 - owp, total - bwp - bbp - owp); ++obp) {
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

// Convert material to a human-readable string like "KKK" or "KPP"
std::string material_string(const Material& m) {
  std::string result;

  // White pieces
  for (int i = 0; i < m.white_queens; ++i) result += 'K';
  for (int i = 0; i < m.back_white_pawns + m.other_white_pawns; ++i) result += 'P';

  // Separator
  result += 'v';

  // Black pieces
  for (int i = 0; i < m.black_queens; ++i) result += 'K';
  for (int i = 0; i < m.back_black_pawns + m.other_black_pawns; ++i) result += 'P';

  return result;
}

// Convert square index (0-31) to notation (1-32)
int to_notation(int sq) {
  return sq + 1;
}

// Print board position with piece locations
void print_position(const Board& b) {
  std::cout << "Position: ";

  // White pieces
  std::cout << "W(";
  bool first = true;
  for (int sq = 0; sq < 32; ++sq) {
    if (b.white & (1u << sq)) {
      if (!first) std::cout << ",";
      first = false;
      if (b.kings & (1u << sq)) {
        std::cout << "K" << to_notation(sq);
      } else {
        std::cout << "P" << to_notation(sq);
      }
    }
  }
  std::cout << ") ";

  // Black pieces
  std::cout << "B(";
  first = true;
  for (int sq = 0; sq < 32; ++sq) {
    if (b.black & (1u << sq)) {
      if (!first) std::cout << ",";
      first = false;
      if (b.kings & (1u << sq)) {
        std::cout << "K" << to_notation(sq);
      } else {
        std::cout << "P" << to_notation(sq);
      }
    }
  }
  std::cout << ")";
  std::cout << std::endl;
}

struct LongestMate {
  Material material;
  std::size_t index;
  DTM dtm;
  int plies;
};

// Show optimal play from a winning position
void show_optimal_play(Board b, int expected_plies) {
  std::set<std::uint64_t> seen_positions;
  std::vector<FullMove> game_moves;
  int ply = 0;

  while (ply < expected_plies + 10) {
    Material cur_m = get_material(b);
    DTM current_dtm = lookup_dtm(b);

    // After a move+flip, if side to move has no pieces, previous side won
    if (cur_m.white_pieces() == 0) {
      // Previous side captured all opponent pieces
      break;
    }

    // Check for repetition
    std::uint64_t pos_hash = b.hash();
    if (seen_positions.count(pos_hash)) {
      std::cout << "(DRAW by repetition)\n";
      break;
    }
    seen_positions.insert(pos_hash);

    std::vector<FullMove> moves;
    generateFullMoves(b, moves);

    if (moves.empty()) {
      // No moves = loss (stalemate is a loss in Spanish checkers)
      break;
    }

    // Find best move based on DTM
    // - For winning (DTM>0): find move giving opponent largest (least negative) loss DTM
    //   because larger (less negative) means faster loss for opponent
    // - For losing (DTM<0): find move giving opponent largest win DTM (slowest win)
    FullMove best_move;
    DTM best_opp_dtm = DTM_UNKNOWN;
    bool found = false;

    for (const auto& mv : moves) {
      Board next = makeMove(b, mv.move);
      Material next_m = get_material(next);

      // Terminal capture
      DTM opp_dtm = (next_m.white_pieces() == 0) ? DTM_LOSS_TERMINAL : lookup_dtm(next);

      if (opp_dtm == DTM_UNKNOWN) continue;

      bool dominated = false;
      if (!found) {
        dominated = false;
      } else if (current_dtm > 0) {
        // Winning: prefer moves where opponent loses, and among those, faster loss
        if (best_opp_dtm < 0 && opp_dtm >= 0) dominated = true;
        else if (best_opp_dtm >= 0 && opp_dtm < 0) dominated = false;
        else if (best_opp_dtm < 0 && opp_dtm < 0) dominated = (opp_dtm <= best_opp_dtm);
        else dominated = true;
      } else if (current_dtm < 0) {
        // Losing: pick move with largest opp_dtm (slowest win for opponent)
        dominated = (opp_dtm <= best_opp_dtm);
      } else {
        dominated = (opp_dtm <= best_opp_dtm);
      }

      if (!dominated) {
        best_opp_dtm = opp_dtm;
        best_move = mv;
        found = true;
      }
    }

    if (!found) break;

    game_moves.push_back(best_move);
    b = makeMove(b, best_move.move);
    ply++;
  }

  // Print the game
  std::cout << gameToString(game_moves) << "\n";

  if ((int)game_moves.size() != expected_plies) {
    std::cout << "(Game length: " << game_moves.size() << " plies, expected " << expected_plies << ")\n";
  }
}

int main(int argc, char* argv[]) {
  int max_pieces = 6;
  bool show_play = true;

  if (argc > 1) {
    max_pieces = std::atoi(argv[1]);
    if (max_pieces < 2 || max_pieces > 8) {
      std::cerr << "Max pieces must be between 2 and 8" << std::endl;
      return 1;
    }
  }
  if (argc > 2 && std::string(argv[2]) == "--no-play") {
    show_play = false;
  }

  std::cout << "Finding longest winning positions for " << max_pieces << "-piece tablebases\n";
  std::cout << std::string(60, '=') << "\n\n";

  std::vector<Material> all = all_materials(max_pieces);

  // Group by piece count, find longest mate for each
  std::map<int, LongestMate> best_by_count;

  for (const Material& m : all) {
    if (!dtm_exists(m)) {
      continue;
    }

    std::vector<DTM> dtm = load_dtm(m);
    if (dtm.empty()) {
      continue;
    }

    // Find position with maximum DTM (longest win)
    DTM max_dtm = 0;
    std::size_t max_idx = 0;

    for (std::size_t idx = 0; idx < dtm.size(); ++idx) {
      if (dtm[idx] > max_dtm) {
        max_dtm = dtm[idx];
        max_idx = idx;
      }
    }

    if (max_dtm > 0) {
      int pieces = m.total_pieces();
      int plies = dtm_to_plies(max_dtm);

      if (best_by_count.find(pieces) == best_by_count.end() ||
          plies > best_by_count[pieces].plies) {
        best_by_count[pieces] = {m, max_idx, max_dtm, plies};
      }
    }
  }

  // Print results for each piece count
  for (int pieces = 2; pieces <= max_pieces; pieces++) {
    std::cout << "=== " << pieces << " PIECES ===\n";

    if (best_by_count.find(pieces) == best_by_count.end()) {
      std::cout << "No winning positions found\n\n";
      continue;
    }

    const LongestMate& lm = best_by_count[pieces];
    Board b = index_to_board(lm.index, lm.material);

    std::cout << "Longest mate: " << lm.dtm << " moves (" << lm.plies << " plies)\n";
    std::cout << "Material: " << material_string(lm.material) << "\n";
    print_position(b);
    std::cout << b;

    if (show_play) {
      std::cout << "\nOptimal play:\n";
      show_optimal_play(b, lm.plies);
    }
    std::cout << "\n";
  }

  return 0;
}
