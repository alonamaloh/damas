#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include "notation.h"
#include <iostream>
#include <unordered_map>
#include <set>
#include <cstdlib>

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
  std::cout << "  W: ";
  for (int sq = 0; sq < 32; ++sq) {
    if (b.white & (1u << sq)) {
      std::cout << (b.kings & (1u << sq) ? "K" : "P") << to_notation(sq) << " ";
    }
  }
  std::cout << " B: ";
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

void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " <material>\n";
  std::cerr << "  material: 6-digit code (back_wp back_bp other_wp other_bp wk bk)\n";
  std::cerr << "\nExamples:\n";
  std::cerr << "  " << prog << " 000031   # KKKvK\n";
  std::cerr << "  " << prog << " 011000   # PvP (longest 2-piece)\n";
  std::cerr << "  " << prog << " 021000   # PvPP (longest 3-piece)\n";
  std::cerr << "  " << prog << " 201100   # PPPvP (longest 4-piece)\n";
  std::cerr << "  " << prog << " 201200   # PPPvPP (longest 5-piece)\n";
  std::cerr << "  " << prog << " 113100   # PPPPvPP (longest 6-piece)\n";
}

// Find best move from a position
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

  Board start_pos = index_to_board(max_idx, m);
  int expected_plies = dtm_to_plies(max_dtm);

  // ========== PHASE 1: Collect all moves ==========
  std::vector<FullMove> game_moves;
  std::set<std::uint64_t> seen_positions;
  Board b = start_pos;
  int ply = 0;
  std::string result_msg;

  while (ply < expected_plies + 10) {
    Material cur_m = get_material(b);
    DTM current_dtm = lookup_dtm(b);

    if (cur_m.white_pieces() == 0) {
      result_msg = (ply % 2 == 1) ? "1-0 White wins" : "0-1 Black wins";
      break;
    }

    std::uint64_t pos_hash = b.hash();
    if (seen_positions.count(pos_hash)) {
      result_msg = "1/2-1/2 Draw by repetition";
      break;
    }
    seen_positions.insert(pos_hash);

    std::vector<FullMove> moves;
    generateFullMoves(b, moves);

    if (moves.empty()) {
      result_msg = (ply % 2 == 0) ? "0-1 White has no moves" : "1-0 Black has no moves";
      break;
    }

    FullMove best_move = find_best_move(b, current_dtm);
    if (best_move.path.empty()) {
      result_msg = "??? No evaluated moves";
      break;
    }

    game_moves.push_back(best_move);
    b = makeMove(b, best_move.move);
    ply++;
  }

  // ========== PHASE 2: Print header, initial position, and game notation ==========
  std::cout << "Material: " << m << " (" << material_string(m) << ")\n";
  std::cout << "Longest mate: " << max_dtm << " moves (" << expected_plies << " plies)\n";
  std::cout << std::string(60, '=') << "\n\n";

  std::cout << "Initial position:\n";
  std::cout << start_pos;
  print_pieces(start_pos);

  std::cout << "\nGame (" << game_moves.size() << " plies):\n";
  std::cout << gameToString(game_moves) << "\n";
  std::cout << result_msg << "\n";

  std::cout << "\n" << std::string(60, '=') << "\n";
  std::cout << "Position-by-position replay:\n";
  std::cout << std::string(60, '=') << "\n\n";

  // ========== PHASE 3: Replay with board positions ==========
  b = start_pos;
  seen_positions.clear();

  for (size_t i = 0; i < game_moves.size(); ++i) {
    bool is_white_turn = (i % 2 == 0);
    int move_num = (i / 2) + 1;
    DTM current_dtm = lookup_dtm(b);

    // Print position header
    if (is_white_turn) {
      std::cout << move_num << ". White to move";
    } else {
      std::cout << move_num << ". ...Black to move";
    }
    std::cout << "  [DTM=" << current_dtm << "]\n";

    // Show board from consistent (original white) perspective
    Board display = is_white_turn ? b : flip(b);
    std::cout << display;
    print_pieces(display);

    // Print the move (flip notation for black's moves)
    std::string move_str;
    if (is_white_turn) {
      move_str = moveToString(game_moves[i]);
    } else {
      FullMove flipped = game_moves[i];
      for (auto& sq : flipped.path) sq = flip_sq(sq);
      move_str = moveToString(flipped);
    }

    std::cout << "\n  Plays: " << move_str << "\n";
    std::cout << std::string(60, '-') << "\n";

    b = makeMove(b, game_moves[i].move);
  }

  // Show final position
  Material final_m = get_material(b);
  bool is_white_turn = (game_moves.size() % 2 == 0);

  if (final_m.white_pieces() > 0 && final_m.black_pieces() > 0) {
    int move_num = (game_moves.size() / 2) + 1;
    DTM final_dtm = lookup_dtm(b);

    if (is_white_turn) {
      std::cout << move_num << ". White to move";
    } else {
      std::cout << move_num << ". ...Black to move";
    }
    std::cout << "  [DTM=" << final_dtm << "]\n";

    Board display = is_white_turn ? b : flip(b);
    std::cout << display;
    print_pieces(display);
  }

  std::cout << "\n*** " << result_msg << " ***\n";

  return 0;
}
