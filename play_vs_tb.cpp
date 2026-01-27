#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include "notation.h"
#include <iostream>
#include <unordered_map>
#include <set>
#include <cstdlib>
#include <algorithm>

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

int to_notation(int sq) { return sq + 1; }
int flip_sq(int sq) { return 31 - sq; }

void print_pieces(const Board& b) {
  std::cout << "  White: ";
  for (int sq = 0; sq < 32; ++sq) {
    if (b.white & (1u << sq)) {
      std::cout << (b.kings & (1u << sq) ? "K" : "P") << to_notation(sq) << " ";
    }
  }
  std::cout << "\n  Black: ";
  for (int sq = 0; sq < 32; ++sq) {
    if (b.black & (1u << sq)) {
      std::cout << (b.kings & (1u << sq) ? "K" : "P") << to_notation(sq) << " ";
    }
  }
  std::cout << "\n";
}

std::string material_string(const Material& m) {
  std::string result;
  for (int i = 0; i < m.white_queens; ++i) result += 'K';
  for (int i = 0; i < m.back_white_pawns + m.other_white_pawns; ++i) result += 'P';
  result += 'v';
  for (int i = 0; i < m.black_queens; ++i) result += 'K';
  for (int i = 0; i < m.back_black_pawns + m.other_black_pawns; ++i) result += 'P';
  return result;
}

std::string dtm_description(DTM d) {
  if (d == DTM_UNKNOWN) return "unknown";
  if (d == DTM_LOSS_TERMINAL) return "checkmate (lost)";
  if (d > 0) return "winning in " + std::to_string(d) + " moves";
  if (d < 0) return "losing in " + std::to_string(-d) + " moves";
  return "draw";
}

void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " <material>\n";
  std::cerr << "  Play as Black (losing side) against tablebase-perfect White.\n";
  std::cerr << "  material: 6-digit code (back_wp back_bp other_wp other_bp wk bk)\n";
  std::cerr << "\nExamples:\n";
  std::cerr << "  " << prog << " 000031   # KKKvK\n";
  std::cerr << "  " << prog << " 011000   # PvP (longest 2-piece)\n";
  std::cerr << "  " << prog << " 021000   # PvPP (longest 3-piece)\n";
  std::cerr << "  " << prog << " 201100   # PPPvP (longest 4-piece)\n";
}

// Find best move for the winning side
FullMove find_best_move(const Board& b, DTM current_dtm) {
  std::vector<FullMove> moves;
  generateFullMoves(b, moves);

  FullMove best_move;
  DTM best_opp_dtm = DTM_UNKNOWN;
  bool found = false;

  for (const auto& mv : moves) {
    Board next = makeMove(b, mv.move);
    Material next_m = get_material(next);
    DTM opp_dtm = (next_m.white_pieces() == 0) ? DTM_LOSS_TERMINAL : lookup_dtm(next);

    if (opp_dtm == DTM_UNKNOWN) continue;

    bool dominated = false;
    if (!found) {
      dominated = false;
    } else if (current_dtm > 0) {
      if (best_opp_dtm < 0 && opp_dtm >= 0) dominated = true;
      else if (best_opp_dtm >= 0 && opp_dtm < 0) dominated = false;
      else if (best_opp_dtm < 0 && opp_dtm < 0) dominated = (opp_dtm <= best_opp_dtm);
      else dominated = true;
    } else if (current_dtm < 0) {
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

  return best_move;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  std::string code = argv[1];
  if (code.size() != 6) {
    std::cerr << "Error: material code must be 6 digits\n";
    usage(argv[0]);
    return 1;
  }

  Material m{
    code[0] - '0', code[1] - '0', code[2] - '0',
    code[3] - '0', code[4] - '0', code[5] - '0'
  };

  if (!dtm_exists(m)) {
    std::cerr << "Error: DTM tablebase not found for " << m << "\n";
    return 1;
  }

  auto dtm_table = load_dtm(m);
  if (dtm_table.empty()) {
    std::cerr << "Error: Failed to load DTM tablebase\n";
    return 1;
  }

  // Find longest mate
  DTM max_dtm = 0;
  std::size_t max_idx = 0;
  for (std::size_t i = 0; i < dtm_table.size(); ++i) {
    if (dtm_table[i] > max_dtm) {
      max_dtm = dtm_table[i];
      max_idx = i;
    }
  }

  if (max_dtm <= 0) {
    std::cout << "No winning positions in this tablebase\n";
    return 0;
  }

  Board b = index_to_board(max_idx, m);
  int expected_plies = dtm_to_plies(max_dtm);

  std::cout << "╔════════════════════════════════════════════════════════════╗\n";
  std::cout << "║  PLAY VS TABLEBASE - You are BLACK (the losing side)       ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";

  std::cout << "Material: " << m << " (" << material_string(m) << ")\n";
  std::cout << "Starting from longest mate position: " << max_dtm << " moves (" << expected_plies << " plies)\n";
  std::cout << "Enter moves like: 29-25 or 9x18 (or 'q' to quit, 'h' for help)\n";
  std::cout << std::string(60, '=') << "\n\n";

  int ply = 0;
  int move_num = 1;

  while (true) {
    Material cur_m = get_material(b);
    DTM current_dtm = lookup_dtm(b);
    bool is_white_turn = (ply % 2 == 0);

    // Check for game over
    if (cur_m.white_pieces() == 0) {
      std::cout << "\n*** BLACK WINS! (captured all white pieces) ***\n";
      break;
    }
    if (cur_m.black_pieces() == 0) {
      std::cout << "\n*** WHITE WINS! (captured all black pieces) ***\n";
      break;
    }

    // Show position (always from original white's perspective)
    Board display = is_white_turn ? b : flip(b);

    std::cout << "┌─────────────────────────────────────────────────────────┐\n";
    if (is_white_turn) {
      std::cout << "│  Move " << move_num << ": WHITE to move\n";
    } else {
      std::cout << "│  Move " << move_num << ": BLACK to move (your turn)\n";
    }
    std::cout << "│  Position evaluation: " << dtm_description(current_dtm) << "\n";
    if (current_dtm > 0) {
      std::cout << "│  (White wins with perfect play in " << dtm_to_plies(current_dtm) << " plies)\n";
    } else if (current_dtm < 0 && current_dtm != DTM_UNKNOWN) {
      std::cout << "│  (You lose with perfect play in " << dtm_to_plies(current_dtm) << " plies)\n";
    }
    std::cout << "└─────────────────────────────────────────────────────────┘\n";

    std::cout << display;
    print_pieces(display);
    std::cout << "\n";

    // Generate moves
    std::vector<FullMove> moves;
    generateFullMoves(b, moves);

    if (moves.empty()) {
      if (is_white_turn) {
        std::cout << "*** WHITE has no moves - BLACK WINS! ***\n";
      } else {
        std::cout << "*** BLACK has no moves - WHITE WINS! ***\n";
      }
      break;
    }

    if (is_white_turn) {
      // Computer plays as White
      FullMove best = find_best_move(b, current_dtm);
      if (best.path.empty()) {
        std::cout << "Error: No valid move found\n";
        break;
      }

      std::string move_str = moveToString(best);

      // Find resulting DTM
      Board next = makeMove(b, best.move);
      DTM next_dtm = lookup_dtm(next);

      std::cout << "  White plays: " << move_str;
      if (best.move.isCapture()) std::cout << " (capture!)";
      std::cout << "\n";
      std::cout << "  Your position is now: " << dtm_description(next_dtm) << "\n";
      std::cout << std::string(60, '-') << "\n\n";

      b = makeMove(b, best.move);
      ply++;
    } else {
      // Human plays as Black - show available moves with evaluations
      std::cout << "Your legal moves:\n";

      // Collect and sort moves by DTM (best defense first)
      std::vector<std::pair<FullMove, DTM>> move_evals;
      for (const auto& mv : moves) {
        Board next = makeMove(b, mv.move);
        Material next_m = get_material(next);
        DTM opp_dtm = (next_m.white_pieces() == 0) ? DTM_LOSS_TERMINAL : lookup_dtm(next);
        move_evals.push_back({mv, opp_dtm});
      }

      // Sort: largest opp_dtm first (slowest win for opponent = best defense)
      std::sort(move_evals.begin(), move_evals.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });

      for (size_t i = 0; i < move_evals.size(); ++i) {
        const auto& [mv, opp_dtm] = move_evals[i];

        // Flip move notation for black's perspective
        FullMove flipped = mv;
        for (auto& sq : flipped.path) sq = flip_sq(sq);
        std::string move_str = moveToString(flipped);

        std::cout << "  " << move_str;
        if (mv.move.isCapture()) std::cout << " (capture)";

        // Show what happens after this move
        if (opp_dtm == DTM_LOSS_TERMINAL) {
          std::cout << " -> White loses immediately!";
        } else if (opp_dtm > 0) {
          std::cout << " -> White wins in " << opp_dtm << " moves";
        } else if (opp_dtm < 0) {
          std::cout << " -> You escape! (White now losing)";
        } else if (opp_dtm == 0) {
          std::cout << " -> Draw!";
        }

        if (i == 0 && opp_dtm > 0) std::cout << " [best defense]";
        std::cout << "\n";
      }

      std::cout << "\nEnter your move: ";
      std::string input;
      if (!std::getline(std::cin, input)) {
        std::cout << "\nGoodbye!\n";
        break;
      }

      // Trim whitespace
      while (!input.empty() && std::isspace(input.front())) input.erase(0, 1);
      while (!input.empty() && std::isspace(input.back())) input.pop_back();

      if (input == "q" || input == "quit") {
        std::cout << "Thanks for playing!\n";
        break;
      }

      if (input == "h" || input == "help") {
        std::cout << "\nHelp:\n";
        std::cout << "  Enter moves like: 29-25 (simple move) or 9x18 (capture)\n";
        std::cout << "  For multi-captures: 9x18x27\n";
        std::cout << "  Commands: q=quit, h=help\n\n";
        continue;
      }

      // Parse the move (from black's perspective, so flip)
      auto parsed = parseMove(b, input, true);  // true = from black perspective
      if (!parsed) {
        std::cout << "  Invalid move! Try again.\n\n";
        continue;
      }

      // Find the DTM after this move
      Board next = makeMove(b, parsed->move);
      DTM next_dtm = lookup_dtm(next);

      std::cout << "\n  You played: " << input << "\n";
      std::cout << "  White's position is now: " << dtm_description(next_dtm) << "\n";
      std::cout << std::string(60, '-') << "\n\n";

      b = makeMove(b, parsed->move);
      ply++;
      move_num++;
    }
  }

  std::cout << "\nGame ended after " << ply << " plies (" << (ply + 1) / 2 << " moves)\n";

  return 0;
}
