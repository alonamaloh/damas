#include "board.h"
#include "movegen.h"
#include "notation.h"
#include "tablebase.h"
#include <iostream>
#include <random>
#include <sstream>

// Simple test framework
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    ++tests_run; \
    std::cout << "  " << #name << "... "; \
    try { \
      test_##name(); \
      ++tests_passed; \
      std::cout << "OK\n"; \
    } catch (const std::exception& e) { \
      std::cout << "FAILED: " << e.what() << "\n"; \
    } \
  } while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
      std::ostringstream oss; \
      oss << "Assertion failed: " #cond " at line " << __LINE__; \
      throw std::runtime_error(oss.str()); \
    } \
  } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
      std::ostringstream oss; \
      oss << "Expected " << (b) << " but got " << (a) << " at line " << __LINE__; \
      throw std::runtime_error(oss.str()); \
    } \
  } while(0)

// =============================================================================
// Board tests
// =============================================================================

TEST(initial_position) {
  Board b;
  ASSERT_EQ(b.white, 0x00000fffu);
  ASSERT_EQ(b.black, 0xfff00000u);
  ASSERT_EQ(b.kings, 0u);
  ASSERT_EQ(b.n_reversible, 0u);
}

TEST(flip_bitboard) {
  // flip(flip(x)) == x
  for (Bb x : {0u, 1u, 0x80000000u, 0x12345678u, 0xffffffffu}) {
    ASSERT_EQ(flip(flip(x)), x);
  }
  // Single bit flips correctly
  ASSERT_EQ(flip(1u << 0), 1u << 31);
  ASSERT_EQ(flip(1u << 31), 1u << 0);
}

TEST(flip_board) {
  Board b;
  Board flipped = flip(b);
  // After flip, white becomes black's pieces (rotated)
  ASSERT_EQ(flipped.white, flip(b.black));
  ASSERT_EQ(flipped.black, flip(b.white));
  // Double flip restores original
  Board restored = flip(flipped);
  ASSERT(restored == b);
}

TEST(direction_functions) {
  // Test that directions work correctly for a center square
  // Square 13 (notation 14) is in row 3
  Bb sq13 = 1u << 13;

  // moveNW should go to square 17 or 18
  Bb nw = moveNW(sq13);
  ASSERT(nw != 0);

  // moveSE should go to square 8 or 9
  Bb se = moveSE(sq13);
  ASSERT(se != 0);

  // Direction functions should be inverses (mostly)
  // moveNW and moveSE, moveNE and moveSW
  for (int sq = 8; sq < 24; ++sq) {  // Middle rows
    Bb bit = 1u << sq;
    Bb nw = moveNW(bit);
    Bb ne = moveNE(bit);
    if (nw) ASSERT(moveSE(nw) & bit);
    if (ne) ASSERT(moveSW(ne) & bit);
  }
}

TEST(board_hash) {
  Board b1, b2;
  // Same boards should have same hash
  ASSERT_EQ(b1.hash(), b2.hash());

  // Different boards should (likely) have different hashes
  b2.white = 0x00000ffeu;
  ASSERT(b1.hash() != b2.hash());
}

// =============================================================================
// Move generation tests
// =============================================================================

TEST(initial_moves) {
  Board b;
  std::vector<Move> moves;
  generateMoves(b, moves);
  // Initial position has 7 legal moves
  ASSERT_EQ(moves.size(), 7u);
}

TEST(perft_depth_1) {
  Board b;
  ASSERT_EQ(perft(b, 1), 7ull);
}

TEST(perft_depth_2) {
  Board b;
  ASSERT_EQ(perft(b, 2), 49ull);
}

TEST(perft_depth_3) {
  Board b;
  ASSERT_EQ(perft(b, 3), 302ull);
}

TEST(perft_depth_4) {
  Board b;
  ASSERT_EQ(perft(b, 4), 1469ull);
}

TEST(perft_depth_5) {
  Board b;
  ASSERT_EQ(perft(b, 5), 7361ull);
}

TEST(perft_depth_6) {
  Board b;
  ASSERT_EQ(perft(b, 6), 36473ull);
}

TEST(perft_depth_7) {
  Board b;
  ASSERT_EQ(perft(b, 7), 177532ull);
}

TEST(perft_depth_8) {
  Board b;
  ASSERT_EQ(perft(b, 8), 828783ull);
}

TEST(circular_capture) {
  // White queen on 2 (internal 1), black pawns on 5, 6, 13, 14
  Board board;
  board.white = 1u << 1;
  board.black = (1u << 4) | (1u << 5) | (1u << 12) | (1u << 13);
  board.kings = 1u << 1;

  std::vector<FullMove> moves;
  generateFullMoves(board, moves);

  // Should have exactly one move
  ASSERT_EQ(moves.size(), 1u);

  // The move should be circular (from == to)
  ASSERT_EQ(moves[0].move.from_xor_to, 0u);

  // Should capture 4 pieces
  ASSERT_EQ(std::popcount(moves[0].move.captures), 4);

  // Path should have 5 elements (start, 4 landings back to start)
  ASSERT_EQ(moves[0].path.size(), 5u);
  ASSERT_EQ(moves[0].path.front(), moves[0].path.back());

  // Apply the move - board is flipped after, so:
  // - after.white is the flipped captured pieces (should be 0)
  // - after.black is the flipped white queen
  Board after = makeMove(board, moves[0].move);
  ASSERT_EQ(after.white, 0u);  // All opponent pieces captured (now shown as white after flip)
}

TEST(ley_de_cantidad) {
  // Position where pawn can capture 1 or 2 pieces
  // Pawn on 9, black pawns on 13, 22
  Board board;
  board.white = 1u << 8;   // Pawn on 9 (internal 8)
  board.black = (1u << 12) | (1u << 21);  // Pawns on 13, 22
  board.kings = 0;

  std::vector<Move> moves;
  generateMoves(board, moves);

  // Should only have the 2-capture move (ley de cantidad)
  ASSERT_EQ(moves.size(), 1u);
  ASSERT_EQ(std::popcount(moves[0].captures), 2);
}

// =============================================================================
// Notation tests
// =============================================================================

TEST(move_to_string_simple) {
  Move m(1u | (1u << 4), 0);  // Square 0 to square 4
  std::string s = moveToString(m);
  ASSERT_EQ(s, "1-5");
}

TEST(move_to_string_capture) {
  Move m(1u | (1u << 8), 1u << 4);  // Square 0 to 8, capturing square 4
  std::string s = moveToString(m);
  ASSERT_EQ(s, "1x9");
}

TEST(fullmove_to_string) {
  FullMove fm;
  fm.move = Move(1u | (1u << 16), (1u << 4) | (1u << 12));
  fm.path = {0, 8, 16};
  std::string s = moveToString(fm);
  ASSERT_EQ(s, "1x9x17");
}

TEST(parse_simple_move) {
  Board b;
  auto move = parseMove(b, "11-15");
  ASSERT(move.has_value());
  ASSERT(!move->move.isCapture());
}

TEST(parse_invalid_move) {
  Board b;
  auto move = parseMove(b, "1-5");  // Invalid from initial position
  ASSERT(!move.has_value());
}

TEST(notation_round_trip) {
  // Generate a random game and verify round-trip
  std::mt19937 rng(12345);
  Board board;
  std::vector<FullMove> gameMoves;
  std::vector<FullMove> moves;

  for (int ply = 0; ply < 50; ++ply) {
    generateFullMoves(board, moves);
    if (moves.empty()) break;

    std::uniform_int_distribution<size_t> dist(0, moves.size() - 1);
    FullMove move = moves[dist(rng)];
    gameMoves.push_back(move);
    board = makeMove(board, move.move);
  }

  // Convert to string and parse back
  std::string gameStr = gameToString(gameMoves);
  Board startBoard;
  auto record = parseGame(startBoard, gameStr);

  ASSERT(record.complete);
  ASSERT_EQ(record.moves.size(), gameMoves.size());
  ASSERT(record.finalBoard == board);
}

TEST(circular_capture_notation) {
  // White queen on 2, black pawns on 5, 6, 13, 14
  Board board;
  board.white = 1u << 1;
  board.black = (1u << 4) | (1u << 5) | (1u << 12) | (1u << 13);
  board.kings = 1u << 1;

  std::vector<FullMove> moves;
  generateFullMoves(board, moves);
  ASSERT_EQ(moves.size(), 1u);

  std::string notation = moveToString(moves[0]);
  // Should show full path like "2x11x18x9x2" or similar
  ASSERT(notation.find('x') != std::string::npos);
  ASSERT(notation.length() > 5);  // More than just "2x2"

  // First and last square should be the same
  size_t firstX = notation.find('x');
  size_t lastX = notation.rfind('x');
  std::string firstSq = notation.substr(0, firstX);
  std::string lastSq = notation.substr(lastX + 1);
  ASSERT_EQ(firstSq, lastSq);
}

// =============================================================================
// Tablebase tests
// =============================================================================

TEST(choose_function) {
  // Basic binomial coefficients
  ASSERT_EQ(choose(4, 0), 1u);
  ASSERT_EQ(choose(4, 1), 4u);
  ASSERT_EQ(choose(4, 2), 6u);
  ASSERT_EQ(choose(4, 4), 1u);
  ASSERT_EQ(choose(24, 1), 24u);
  ASSERT_EQ(choose(24, 2), 276u);
  ASSERT_EQ(choose(32, 2), 496u);
}

TEST(material_from_board) {
  // Initial position
  Board b;
  Material m = get_material(b);
  ASSERT_EQ(m.back_white_pawns, 4);
  ASSERT_EQ(m.back_black_pawns, 4);
  ASSERT_EQ(m.other_white_pawns, 8);
  ASSERT_EQ(m.other_black_pawns, 8);
  ASSERT_EQ(m.white_queens, 0);
  ASSERT_EQ(m.black_queens, 0);
}

TEST(material_kvk) {
  // Queen vs Queen on specific squares
  Board b;
  b.white = 1u << 10;  // White queen on square 10
  b.black = 1u << 20;  // Black queen on square 20
  b.kings = b.white | b.black;

  Material m = get_material(b);
  ASSERT_EQ(m.back_white_pawns, 0);
  ASSERT_EQ(m.back_black_pawns, 0);
  ASSERT_EQ(m.other_white_pawns, 0);
  ASSERT_EQ(m.other_black_pawns, 0);
  ASSERT_EQ(m.white_queens, 1);
  ASSERT_EQ(m.black_queens, 1);
}

TEST(material_size_kvk) {
  // Queen vs Queen: C(32,1) * C(31,1) = 32 * 31 = 992
  Material m{0, 0, 0, 0, 1, 1};
  ASSERT_EQ(material_size(m), 992u);
}

TEST(material_size_kvp) {
  // Queen vs pawn in middle: C(24,1) * C(31,1) = 24 * 31 = 744
  // (pawn takes 1 of 24 middle squares, queen gets 31 of remaining 32)
  Material m{0, 0, 0, 1, 1, 0};
  ASSERT_EQ(material_size(m), 744u);
}

TEST(indexing_roundtrip_kvk) {
  // Test all positions in KvK endgame
  Material m{0, 0, 0, 0, 1, 1};
  std::size_t size = material_size(m);

  for (std::size_t idx = 0; idx < size; ++idx) {
    Board b = index_to_board(idx, m);

    // Verify material
    Material extracted = get_material(b);
    ASSERT(extracted == m);

    // Verify index round-trip
    std::size_t idx2 = board_to_index(b, m);
    ASSERT_EQ(idx, idx2);
  }
}

TEST(indexing_roundtrip_kvpp) {
  // Test some positions in K vs 2 pawns endgame
  Material m{0, 0, 0, 2, 1, 0};
  std::size_t size = material_size(m);

  // Test first 100, last 100, and some random positions
  std::mt19937 rng(42);
  std::vector<std::size_t> test_indices;

  for (std::size_t i = 0; i < std::min(size, std::size_t(100)); ++i)
    test_indices.push_back(i);
  for (std::size_t i = size > 100 ? size - 100 : 0; i < size; ++i)
    test_indices.push_back(i);
  for (int i = 0; i < 100 && size > 200; ++i)
    test_indices.push_back(rng() % size);

  for (std::size_t idx : test_indices) {
    Board b = index_to_board(idx, m);
    Material extracted = get_material(b);
    ASSERT(extracted == m);
    std::size_t idx2 = board_to_index(b, m);
    ASSERT_EQ(idx, idx2);
  }
}

TEST(indexing_with_back_pawns) {
  // Test material with back row pawns
  Material m{1, 1, 0, 0, 1, 1};  // 1 back pawn each + 1 queen each
  std::size_t size = material_size(m);

  // C(4,1) * C(4,1) * C(30,1) * C(29,1) = 4 * 4 * 30 * 29 = 13920
  ASSERT_EQ(size, 13920u);

  // Test round-trip on sample positions
  std::mt19937 rng(123);
  for (int i = 0; i < 100; ++i) {
    std::size_t idx = rng() % size;
    Board b = index_to_board(idx, m);
    Material extracted = get_material(b);
    ASSERT(extracted == m);
    std::size_t idx2 = board_to_index(b, m);
    ASSERT_EQ(idx, idx2);
  }
}

TEST(material_flip) {
  Material m{1, 2, 3, 4, 5, 6};
  Material f = flip(m);
  ASSERT_EQ(f.back_white_pawns, 2);
  ASSERT_EQ(f.back_black_pawns, 1);
  ASSERT_EQ(f.other_white_pawns, 4);
  ASSERT_EQ(f.other_black_pawns, 3);
  ASSERT_EQ(f.white_queens, 6);
  ASSERT_EQ(f.black_queens, 5);

  // Double flip restores original
  Material ff = flip(f);
  ASSERT(ff == m);
}

// =============================================================================
// Main
// =============================================================================

int main() {
  std::cout << "Running board tests:\n";
  RUN_TEST(initial_position);
  RUN_TEST(flip_bitboard);
  RUN_TEST(flip_board);
  RUN_TEST(direction_functions);
  RUN_TEST(board_hash);

  std::cout << "\nRunning move generation tests:\n";
  RUN_TEST(initial_moves);
  RUN_TEST(perft_depth_1);
  RUN_TEST(perft_depth_2);
  RUN_TEST(perft_depth_3);
  RUN_TEST(perft_depth_4);
  RUN_TEST(perft_depth_5);
  RUN_TEST(perft_depth_6);
  RUN_TEST(perft_depth_7);
  RUN_TEST(perft_depth_8);
  RUN_TEST(circular_capture);
  RUN_TEST(ley_de_cantidad);

  std::cout << "\nRunning notation tests:\n";
  RUN_TEST(move_to_string_simple);
  RUN_TEST(move_to_string_capture);
  RUN_TEST(fullmove_to_string);
  RUN_TEST(parse_simple_move);
  RUN_TEST(parse_invalid_move);
  RUN_TEST(notation_round_trip);
  RUN_TEST(circular_capture_notation);

  std::cout << "\nRunning tablebase tests:\n";
  RUN_TEST(choose_function);
  RUN_TEST(material_from_board);
  RUN_TEST(material_kvk);
  RUN_TEST(material_size_kvk);
  RUN_TEST(material_size_kvp);
  RUN_TEST(indexing_roundtrip_kvk);
  RUN_TEST(indexing_roundtrip_kvpp);
  RUN_TEST(indexing_with_back_pawns);
  RUN_TEST(material_flip);

  std::cout << "\n========================================\n";
  std::cout << "Tests: " << tests_passed << "/" << tests_run << " passed\n";

  return (tests_passed == tests_run) ? 0 : 1;
}
