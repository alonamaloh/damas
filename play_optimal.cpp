#include "tablebase.h"
#include "board.h"
#include "movegen.h"
#include "notation.h"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <algorithm>

// Cache for DTM tablebases
std::unordered_map<Material, std::vector<DTM>> dtm_cache;

DTM lookup_dtm(const Board& b) {
  Material m = get_material(b);

  // Check for terminal positions
  if (m.white_pieces() == 0) return DTM_LOSS_TERMINAL;
  if (m.black_pieces() == 0) return DTM_LOSS_TERMINAL;  // From opponent's view after flip

  if (dtm_cache.find(m) == dtm_cache.end()) {
    dtm_cache[m] = load_dtm(m);
  }
  auto& tb = dtm_cache[m];
  if (tb.empty()) return DTM_UNKNOWN;
  std::size_t idx = board_to_index(b, m);
  if (idx >= tb.size()) return DTM_UNKNOWN;
  return tb[idx];
}

// Convert square index (0-31) to notation (1-32)
int to_notation(int sq) {
  return sq + 1;
}

// Print board position with piece locations
void print_position(const Board& b, const char* side) {
  std::cout << side << ": ";

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
}

const char* dtm_result(DTM d) {
  if (d > 0) return "WIN";
  if (d < 0 && d != DTM_UNKNOWN) return "LOSS";
  if (d == DTM_UNKNOWN) return "UNKNOWN";
  return "DRAW";
}

// Print board from original white's perspective
// If it's black's turn (odd ply), we need to flip for display
void print_board_consistent(const Board& b, bool white_to_move) {
  if (white_to_move) {
    std::cout << b;
  } else {
    // Flip board so we see it from original white's perspective
    Board display = flip(b);
    std::cout << display;
  }
}

int main() {
  // Set up position: W(K1,K5,K9) B(K6,K15,P26)
  // K1 = square 0, K5 = square 4, K9 = square 8
  // K6 = square 5, K15 = square 14, P26 = square 25
  Board b;
  b.white = (1u << 0) | (1u << 4) | (1u << 8);   // K1, K5, K9
  b.black = (1u << 5) | (1u << 14) | (1u << 25); // K6, K15, P26
  b.kings = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 5) | (1u << 14);  // All kings except P26
  b.n_reversible = 0;

  std::cout << "=== Optimal Play from W(K1,K5,K9) B(K6,K15,P26) ===" << std::endl;
  std::cout << "Legend: O/o = White King/Pawn, X/x = Black King/Pawn" << std::endl;
  std::cout << std::endl;

  int ply = 0;
  bool white_to_move = true;  // Track original side (before flips)

  while (true) {
    // Calculate move number (1-indexed, increments after black moves)
    int move_number = (ply / 2) + 1;

    // Print current position with move number
    if (white_to_move) {
      std::cout << "Move " << move_number << ". White to move" << std::endl;
    } else {
      std::cout << "Move " << move_number << ". ...Black to move" << std::endl;
    }

    // Print position description (from original perspective)
    Board display = white_to_move ? b : flip(b);
    print_position(display, "Position");

    DTM current_dtm = lookup_dtm(b);
    std::cout << "  DTM=" << current_dtm << " (" << dtm_result(current_dtm)
              << " for " << (white_to_move ? "White" : "Black") << ")" << std::endl;
    print_board_consistent(b, white_to_move);

    // Generate moves
    std::vector<Move> moves;
    generateMoves(b, moves);

    if (moves.empty()) {
      std::cout << (white_to_move ? "White" : "Black") << " has no legal moves and loses!" << std::endl;
      std::cout << (white_to_move ? "Black" : "White") << " wins!" << std::endl;
      break;
    }

    // Check for terminal capture
    Material m = get_material(b);
    if (m.white_pieces() == 0 || m.black_pieces() == 0) {
      std::cout << "Game over - one side has no pieces!" << std::endl;
      break;
    }

    // Find the best move
    // For WIN positions: pick move leading to opponent's quickest LOSS (highest/least negative DTM)
    // For LOSS positions: pick move leading to opponent's slowest WIN (highest positive DTM)
    // For DRAW positions: pick move leading to DRAW

    Move best_move;
    DTM best_succ_dtm = DTM_UNKNOWN;
    bool found_move = false;

    std::cout << "  Analyzing " << moves.size() << " moves:" << std::endl;

    for (const Move& move : moves) {
      Board next = makeMove(b, move);
      Material next_m = get_material(next);

      DTM succ_dtm;
      if (next_m.white_pieces() == 0) {
        // We captured all opponent pieces - terminal loss for them
        succ_dtm = DTM_LOSS_TERMINAL;
      } else {
        succ_dtm = lookup_dtm(next);
      }

      // Get move notation
      std::vector<FullMove> full_moves;
      generateFullMoves(b, full_moves);
      std::string move_str = "?";
      for (const FullMove& fm : full_moves) {
        if (fm.move.from_xor_to == move.from_xor_to && fm.move.captures == move.captures) {
          move_str = moveToString(fm);
          break;
        }
      }

      std::cout << "    " << move_str << " -> DTM=" << succ_dtm
                << " (" << dtm_result(succ_dtm) << ")" << std::endl;

      // Selection logic based on current position's evaluation
      if (current_dtm > 0) {
        // We're winning - find quickest win (opponent's quickest loss)
        // Opponent losses are negative; highest (least negative) = quickest loss
        // DTM_LOSS_TERMINAL (-128) is immediate, so treat specially
        if (succ_dtm < 0 && succ_dtm != DTM_UNKNOWN) {
          if (!found_move) {
            best_move = move;
            best_succ_dtm = succ_dtm;
            found_move = true;
          } else if (succ_dtm == DTM_LOSS_TERMINAL) {
            // Immediate win is always best
            best_move = move;
            best_succ_dtm = succ_dtm;
          } else if (best_succ_dtm != DTM_LOSS_TERMINAL && succ_dtm > best_succ_dtm) {
            // Higher (less negative) = faster loss for opponent
            best_move = move;
            best_succ_dtm = succ_dtm;
          }
        }
      } else if (current_dtm < 0 && current_dtm != DTM_UNKNOWN) {
        // We're losing - find slowest loss (opponent's slowest win)
        // Opponent wins are positive; highest = slowest win for them
        if (succ_dtm > 0) {
          if (!found_move || succ_dtm > best_succ_dtm) {
            best_move = move;
            best_succ_dtm = succ_dtm;
            found_move = true;
          }
        }
      } else {
        // Draw or unknown - pick any draw, or just first move
        if (!found_move) {
          best_move = move;
          best_succ_dtm = succ_dtm;
          found_move = true;
        } else if (succ_dtm == DTM_DRAW && best_succ_dtm != DTM_DRAW) {
          best_move = move;
          best_succ_dtm = succ_dtm;
        }
      }
    }

    if (!found_move) {
      std::cout << "No valid move found!" << std::endl;
      break;
    }

    // Get best move notation
    std::vector<FullMove> full_moves;
    generateFullMoves(b, full_moves);
    std::string best_move_str = "?";
    for (const FullMove& fm : full_moves) {
      if (fm.move.from_xor_to == best_move.from_xor_to && fm.move.captures == best_move.captures) {
        best_move_str = moveToString(fm);
        break;
      }
    }

    std::cout << "  Best move: " << best_move_str << " (successor DTM=" << best_succ_dtm << ")" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    // Make the move
    b = makeMove(b, best_move);
    white_to_move = !white_to_move;
    ply++;

    // Safety check
    if (ply > 500) {
      std::cout << "Stopping after 500 plies" << std::endl;
      break;
    }
  }

  std::cout << "\nGame ended after " << ply << " plies" << std::endl;

  return 0;
}
