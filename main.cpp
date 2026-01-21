#include "board.h"
#include "movegen.h"
#include "notation.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

void testCircularCapture() {
  std::cout << "=== Circular Capture Test (from == to) ===\n\n";

  // White queen on 2 (internal 1), black pawns on 5, 6, 13, 14 (internal 4, 5, 12, 13)
  Board board;
  board.white = 1u << 1;  // Queen on square 1 (notation 2)
  board.black = (1u << 4) | (1u << 5) | (1u << 12) | (1u << 13);  // Pawns on 5,6,13,14
  board.kings = 1u << 1;  // The white piece is a queen

  std::cout << "Position: White queen on 2, black pawns on 5, 6, 13, 14\n";
  std::cout << board << '\n';

  std::vector<FullMove> moves;
  generateFullMoves(board, moves);

  std::cout << "Generated " << moves.size() << " moves:\n";
  for (const auto& m : moves) {
    std::cout << "  from_xor_to=0x" << std::hex << m.move.from_xor_to << std::dec
              << " captures=0x" << std::hex << m.move.captures << std::dec
              << " (" << std::popcount(m.move.captures) << " pieces)";
    if (m.move.from_xor_to == 0) {
      std::cout << " <-- CIRCULAR (from == to)";
    }
    std::cout << " -> " << moveToString(m) << '\n';
  }

  // Find circular moves and test them
  for (const auto& m : moves) {
    if (m.move.from_xor_to == 0) {
      std::cout << "\nTesting circular move application:\n";
      std::cout << "Before:\n" << board;
      Board after = makeMove(board, m.move);
      std::cout << "After:\n" << after;

      // Verify queen is still there and captures removed
      if (after.black == 0) {
        std::cout << "All black pieces captured - CORRECT!\n";
      }
    }
  }
  std::cout << '\n';
}

void testNotation() {
  std::cout << "=== Notation Round-Trip Test ===\n\n";

  // Generate a random game
  std::mt19937 rng(42);  // Fixed seed for reproducibility
  Board board;
  std::vector<FullMove> gameMoves;
  std::vector<FullMove> moves;
  int ply = 0;

  std::cout << "Generating random game...\n";
  while (true) {
    generateFullMoves(board, moves);
    if (moves.empty()) break;

    // Pick a random move
    std::uniform_int_distribution<size_t> dist(0, moves.size() - 1);
    FullMove move = moves[dist(rng)];
    gameMoves.push_back(move);
    board = makeMove(board, move.move);
    ++ply;

    // Limit game length to avoid infinite games
    if (ply >= 200) break;
  }

  std::cout << "Game ended after " << ply << " moves.\n";
  if (moves.empty()) {
    std::cout << "Result: " << (ply % 2 == 0 ? "Black wins" : "White wins") << "\n";
  } else {
    std::cout << "Game truncated at 200 moves.\n";
  }

  // Convert to string notation
  std::string gameStr = gameToString(gameMoves);
  std::cout << "\nGame notation (" << gameStr.size() << " chars):\n";
  // Print first 200 chars
  if (gameStr.size() <= 200) {
    std::cout << gameStr << "\n";
  } else {
    std::cout << gameStr.substr(0, 200) << "...\n";
  }

  // Parse it back
  std::cout << "\nParsing game back...\n";
  Board startBoard;
  auto record = parseGame(startBoard, gameStr);

  if (record.complete) {
    std::cout << "Parsed " << record.moves.size() << " moves successfully.\n";

    // Verify final position matches
    if (record.finalBoard == board) {
      std::cout << "Final position MATCHES!\n";
    } else {
      std::cout << "Final position MISMATCH!\n";
      std::cout << "\nExpected:\n" << board;
      std::cout << "\nGot:\n" << record.finalBoard;
    }
  } else {
    std::cout << "Parse error at move " << record.moves.size() + 1 << ": "
              << record.error << "\n";
  }

  std::cout << "\nFinal position:\n" << board << '\n';
}

void runPerft() {
  std::cout << "=== Perft Test ===\n\n";

  Board board;
  std::cout << "Initial position hash: 0x" << std::hex << board.hash() << std::dec << "\n\n";

  auto totalStart = std::chrono::high_resolution_clock::now();

  for (int depth = 1; depth <= 10; ++depth) {
    auto start = std::chrono::high_resolution_clock::now();
    uint64_t nodes = perft(board, depth);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Depth " << depth << ": " << nodes << " nodes";
    if (ms > 0)
      std::cout << " (" << ms << " ms, " << (nodes * 1000 / ms) << " nps)";
    std::cout << '\n';
  }

  auto totalEnd = std::chrono::high_resolution_clock::now();
  auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
  std::cout << "\nTotal time: " << totalMs << " ms\n";
}

int main() {
  testCircularCapture();
  testNotation();
  runPerft();
  return 0;
}
