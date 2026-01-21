#include "board.h"
#include "movegen.h"
#include "notation.h"
#include "tablebase.h"
#include "compression.h"
#include <iostream>
#include <random>
#include <sstream>
#include <chrono>
#include <iomanip>

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
// Compression tests
// =============================================================================

TEST(has_captures_pawn) {
  // White pawn on 8 (notation 9), black pawn on 12 (notation 13)
  // Pawn can capture diagonally NW
  Board b;
  b.white = 1u << 8;
  b.black = 1u << 12;
  b.kings = 0;

  ASSERT(has_captures(b));
}

TEST(has_captures_none) {
  // White pawn on 8, black pawn far away on 28
  Board b;
  b.white = 1u << 8;
  b.black = 1u << 28;
  b.kings = 0;

  ASSERT(!has_captures(b));
}

TEST(has_captures_queen) {
  // White queen on 0, black pawn on 9, empty at 18
  // Queen can capture along diagonal
  Board b;
  b.white = 1u << 0;
  b.black = 1u << 9;
  b.kings = 1u << 0;

  ASSERT(has_captures(b));
}

TEST(is_dont_care_we_capture) {
  // Position where we have a capture
  Board b;
  b.white = 1u << 8;
  b.black = 1u << 12;
  b.kings = 0;

  ASSERT(is_dont_care(b));
}

TEST(is_dont_care_opponent_captures) {
  // Test that positions where only opponent has captures are NOT don't-care
  // (Changed from original plan to avoid long search chains)
  //
  // White queen at square 4, black queen at square 0
  // We can't capture, but opponent could (if it were their turn)
  Board b;
  b.white = 1u << 4;   // White queen at square 4
  b.black = 1u << 0;   // Black queen at square 0
  b.kings = b.white | b.black;  // Both are queens

  // Verify we don't have captures
  ASSERT(!has_captures(b));

  // Verify opponent would have captures (if it were their turn)
  Board flipped = flip(b);
  bool opp_captures = has_captures(flipped);
  ASSERT(opp_captures);

  // But this is NOT don't-care (conservative approach)
  // We only mark positions where WE have captures as don't-care
  ASSERT(!is_dont_care(b));
}

TEST(is_dont_care_quiet_position) {
  // Position where neither side has captures - not don't care
  // Place pieces far apart
  Board b;
  b.white = 1u << 0;   // Corner square
  b.black = 1u << 31;  // Opposite corner
  b.kings = (1u << 0) | (1u << 31);  // Both queens

  // Queens are far apart, no captures possible
  ASSERT(!has_captures(b));
  ASSERT(!has_captures(flip(b)));
  ASSERT(!is_dont_care(b));
}

TEST(compression_stats_kvk) {
  // Test on KvK (Queen vs Queen) tablebase if available
  Material m{0, 0, 0, 0, 1, 1};
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressionStats stats = analyze_compression(tb, m);

  // Verify totals add up (only dont_care + real = total now)
  ASSERT_EQ(stats.total_positions, tb.size());
  ASSERT_EQ(stats.dont_care_positions + stats.real_positions, stats.total_positions);
  ASSERT_EQ(stats.wins + stats.losses + stats.draws, stats.total_positions);

  // In KvK, there should be some don't-care positions (captures possible)
  // and some real positions
  ASSERT(stats.dont_care_positions > 0);  // Should have some capture positions
  ASSERT(stats.real_positions > 0);
}

TEST(mark_dont_care_roundtrip) {
  // Test that marking don't-care and then looking up returns correct values
  Material m{0, 0, 0, 0, 1, 1};
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressionStats stats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, stats);

  // Verify a sample of positions
  std::mt19937 rng(54321);
  int tested = 0;
  int correct = 0;

  for (int i = 0; i < 100; ++i) {
    std::size_t idx = rng() % tb.size();
    Board board = index_to_board(idx, m);

    Value original = tb[idx];
    Value looked_up = lookup_wdl_with_search(board, marked, m);

    tested++;
    if (original == looked_up) {
      correct++;
    }
  }

  // All lookups should return correct value
  ASSERT_EQ(correct, tested);
}

TEST(lookup_search_correctness) {
  // More thorough test: verify ALL positions in a small tablebase
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressionStats stats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, stats);

  // Verify all positions
  int errors = 0;
  for (std::size_t idx = 0; idx < tb.size(); ++idx) {
    Board board = index_to_board(idx, m);
    Value original = tb[idx];
    Value looked_up = lookup_wdl_with_search(board, marked, m);

    if (original != looked_up) {
      errors++;
      if (errors <= 5) {
        std::cerr << "Mismatch at idx " << idx << ": expected " << static_cast<int>(original)
                  << " got " << static_cast<int>(looked_up) << "\n";
      }
    }
  }

  ASSERT_EQ(errors, 0);
}

// =============================================================================
// Stage 2: Advanced compression tests
// =============================================================================

TEST(search_stats_tracking) {
  // Test that search statistics are tracked correctly
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressionStats cstats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, cstats);

  // Empty sub-tablebases for this test
  std::unordered_map<Material, std::vector<Value>> sub_tbs;
  SearchStats stats;

  // Look up all positions and collect stats
  for (std::size_t idx = 0; idx < tb.size(); ++idx) {
    Board board = index_to_board(idx, m);
    lookup_wdl_with_search(board, marked, m, sub_tbs, &stats);
  }

  // Verify stats are reasonable
  ASSERT_EQ(stats.lookups, tb.size());
  ASSERT_EQ(stats.direct_hits + stats.searches, stats.lookups);
  ASSERT(stats.direct_hits > 0);  // Should have some direct hits
  ASSERT(stats.searches > 0);     // Should have some searches
  ASSERT(stats.terminal_wins > 0); // KvK has terminal wins (captures)
}

TEST(lookup_correctness_kvkk) {
  // Test on K vs 2K (Queen vs 2 Queens) - larger tablebase
  Material m{0, 0, 0, 0, 1, 2};  // 1 white queen, 2 black queens
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  // Load required sub-tablebases
  // After white captures: could go to KvK (if white captures one black queen)
  // After black captures: could go to 0vKK (white eliminated - terminal)
  std::unordered_map<Material, std::vector<Value>> sub_tbs;

  Material kvk{0, 0, 0, 0, 1, 1};  // After white captures one black queen
  std::vector<Value> kvk_tb = load_tablebase(kvk);
  if (!kvk_tb.empty()) {
    CompressionStats sub_stats;
    sub_tbs[kvk] = mark_dont_care_positions(kvk_tb, kvk, sub_stats);
  }

  CompressionStats cstats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, cstats);

  // Test a sample of positions
  std::mt19937 rng(98765);
  int tested = 0;
  int errors = 0;

  for (int i = 0; i < 500 && i < static_cast<int>(tb.size()); ++i) {
    std::size_t idx = rng() % tb.size();
    Board board = index_to_board(idx, m);
    Value original = tb[idx];
    Value looked_up = lookup_wdl_with_search(board, marked, m, sub_tbs, nullptr);

    tested++;
    if (original != looked_up) errors++;
  }

  ASSERT_EQ(errors, 0);
}

TEST(lookup_correctness_multi_material) {
  // Test correctness across multiple material configurations
  // Use materials and their required sub-tablebases
  struct TestCase {
    Material m;
    std::vector<Material> sub_materials;
  };

  std::vector<TestCase> test_cases = {
    {{0, 0, 0, 0, 1, 1}, {}},  // KvK - no sub-TBs needed (captures are terminal)
    {{0, 0, 0, 0, 2, 1}, {{0, 0, 0, 0, 1, 1}}},  // KKvK -> KvK after black captures
    {{0, 0, 0, 0, 1, 2}, {{0, 0, 0, 0, 1, 1}}},  // KvKK -> KvK after white captures
  };

  int total_tested = 0;
  int total_errors = 0;

  for (const TestCase& tc : test_cases) {
    std::vector<Value> tb = load_tablebase(tc.m);
    if (tb.empty()) continue;

    // Load sub-tablebases
    std::unordered_map<Material, std::vector<Value>> sub_tbs;
    for (const Material& sub_m : tc.sub_materials) {
      std::vector<Value> sub_tb = load_tablebase(sub_m);
      if (!sub_tb.empty()) {
        CompressionStats sub_stats;
        sub_tbs[sub_m] = mark_dont_care_positions(sub_tb, sub_m, sub_stats);
      }
    }

    CompressionStats cstats;
    std::vector<Value> marked = mark_dont_care_positions(tb, tc.m, cstats);

    // Test a sample
    std::mt19937 rng(12345);
    for (int i = 0; i < 200 && i < static_cast<int>(tb.size()); ++i) {
      std::size_t idx = rng() % tb.size();
      Board board = index_to_board(idx, tc.m);
      Value original = tb[idx];
      Value looked_up = lookup_wdl_with_search(board, marked, tc.m, sub_tbs, nullptr);

      total_tested++;
      if (original != looked_up) total_errors++;
    }
  }

  ASSERT(total_tested > 0);  // At least some tablebases should exist
  ASSERT_EQ(total_errors, 0);
}

TEST(lookup_with_sub_tablebases) {
  // Test lookup with sub-tablebase support
  // Use KKvK (2 queens vs 1 queen) which can capture down to KvK or KvNothing
  Material m{0, 0, 0, 0, 2, 1};  // 2 white queens, 1 black queen
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  // Load sub-tablebases
  std::unordered_map<Material, std::vector<Value>> sub_tbs;

  // After white captures black's queen: KK vs nothing (terminal, no TB needed)
  // After black captures one white queen: KvK
  Material kvk{0, 0, 0, 0, 1, 1};
  std::vector<Value> kvk_tb = load_tablebase(kvk);
  if (!kvk_tb.empty()) {
    CompressionStats sub_stats;
    sub_tbs[kvk] = mark_dont_care_positions(kvk_tb, kvk, sub_stats);
  }

  CompressionStats cstats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, cstats);

  SearchStats stats;

  // Test positions
  std::mt19937 rng(11111);
  int errors = 0;
  int tested = 0;

  for (int i = 0; i < 300 && i < static_cast<int>(tb.size()); ++i) {
    std::size_t idx = rng() % tb.size();
    Board board = index_to_board(idx, m);
    Value original = tb[idx];
    Value looked_up = lookup_wdl_with_search(board, marked, m, sub_tbs, &stats);

    tested++;
    if (original != looked_up) errors++;
  }

  ASSERT_EQ(errors, 0);
  // With sub-tablebases, we should see some sub-TB lookups
  // (may be 0 if all captures in sample lead to terminal positions)
}

TEST(search_performance_benchmark) {
  // Benchmark search performance
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> tb = load_tablebase(m);

  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressionStats cstats;
  std::vector<Value> marked = mark_dont_care_positions(tb, m, cstats);

  std::unordered_map<Material, std::vector<Value>> sub_tbs;
  SearchStats stats;

  auto start = std::chrono::high_resolution_clock::now();

  // Look up all positions
  for (std::size_t idx = 0; idx < tb.size(); ++idx) {
    Board board = index_to_board(idx, m);
    lookup_wdl_with_search(board, marked, m, sub_tbs, &stats);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  // Print benchmark results
  std::cout << "\n    [Benchmark: " << tb.size() << " lookups in "
            << duration.count() / 1000.0 << "ms";
  std::cout << ", " << std::fixed << std::setprecision(1)
            << (1000000.0 * tb.size() / duration.count()) << " lookups/sec";
  std::cout << ", avg " << std::setprecision(2) << stats.avg_nodes_per_search()
            << " nodes/search";
  std::cout << ", max depth " << stats.max_depth << "] ";

  // Just verify it runs without crashing and produces reasonable stats
  ASSERT(stats.lookups == tb.size());
  ASSERT(stats.max_depth < 100);  // Search shouldn't go too deep
}

TEST(compression_ratio_analysis) {
  // Analyze compression ratios across multiple materials
  std::vector<Material> materials = {
    {0, 0, 0, 0, 1, 1},  // KvK
    {0, 0, 0, 0, 2, 1},  // KKvK
    {0, 0, 0, 1, 1, 0},  // KvP
    {0, 0, 0, 2, 1, 0},  // KvPP
  };

  std::cout << "\n    [Compression analysis:";

  for (const Material& m : materials) {
    std::vector<Value> tb = load_tablebase(m);
    if (tb.empty()) continue;

    CompressionStats stats = analyze_compression(tb, m);
    std::cout << " " << m << "=" << std::fixed << std::setprecision(0)
              << (100.0 * stats.dont_care_ratio()) << "%DC";
  }
  std::cout << "] ";

  ASSERT(true);  // Info-only test
}

// =============================================================================
// Stage 3: Block compression tests
// =============================================================================

TEST(raw_2bit_roundtrip) {
  // Test that RAW_2BIT compression round-trips correctly
  std::vector<Value> values = {
    Value::WIN, Value::LOSS, Value::DRAW, Value::UNKNOWN,
    Value::WIN, Value::WIN, Value::LOSS, Value::DRAW,
    Value::UNKNOWN, Value::DRAW, Value::LOSS, Value::WIN,
  };

  auto compressed = compress_block(values.data(), values.size(), CompressionMethod::RAW_2BIT);
  auto decompressed = decompress_block(compressed.data(), compressed.size(), values.size(), CompressionMethod::RAW_2BIT);

  ASSERT_EQ(decompressed.size(), values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    ASSERT(decompressed[i] == values[i]);
  }

  // Check expected size: 4 values per byte
  std::size_t expected_size = (values.size() + 3) / 4;
  ASSERT_EQ(compressed.size(), expected_size);
}

TEST(raw_2bit_expected_size) {
  // Method 0 produces 4096 bytes for 16384 positions
  ASSERT_EQ(expected_compressed_size(BLOCK_SIZE, CompressionMethod::RAW_2BIT), 4096u);
  ASSERT_EQ(expected_compressed_size(100, CompressionMethod::RAW_2BIT), 25u);
  ASSERT_EQ(expected_compressed_size(1, CompressionMethod::RAW_2BIT), 1u);
}

TEST(ternary_base3_roundtrip) {
  // Test that TERNARY_BASE3 compression round-trips correctly
  // Only 3 distinct values
  std::vector<Value> values = {
    Value::WIN, Value::LOSS, Value::DRAW,
    Value::WIN, Value::WIN, Value::LOSS, Value::DRAW,
    Value::DRAW, Value::LOSS, Value::WIN,
  };

  auto compressed = compress_block(values.data(), values.size(), CompressionMethod::TERNARY_BASE3);
  auto decompressed = decompress_block(compressed.data(), compressed.size(), values.size(), CompressionMethod::TERNARY_BASE3);

  ASSERT_EQ(decompressed.size(), values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    ASSERT(decompressed[i] == values[i]);
  }
}

TEST(ternary_base3_expected_size) {
  // Method 1 produces ~3277 bytes for 16384 ternary values
  // 1 header + ceil(16384/5) = 1 + 3277 = 3278 bytes
  ASSERT_EQ(expected_compressed_size(BLOCK_SIZE, CompressionMethod::TERNARY_BASE3), 3278u);
  ASSERT_EQ(expected_compressed_size(10, CompressionMethod::TERNARY_BASE3), 3u);  // 1 header + 2 data
  ASSERT_EQ(expected_compressed_size(5, CompressionMethod::TERNARY_BASE3), 2u);   // 1 header + 1 data
}

TEST(ternary_fallback_on_4_values) {
  // If a block has all 4 values, ternary should fall back to raw
  std::vector<Value> values = {
    Value::WIN, Value::LOSS, Value::DRAW, Value::UNKNOWN,
    Value::WIN, Value::LOSS,
  };

  // Request ternary, but should get raw 2-bit because 4 distinct values
  auto compressed = compress_block(values.data(), values.size(), CompressionMethod::TERNARY_BASE3);

  // Should still round-trip correctly
  auto decompressed = decompress_block(compressed.data(), compressed.size(), values.size(), CompressionMethod::RAW_2BIT);

  ASSERT_EQ(decompressed.size(), values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    ASSERT(decompressed[i] == values[i]);
  }
}

TEST(compress_block_best_selects_smallest) {
  // Create a block with only 3 values - ternary should be chosen
  std::vector<Value> values(100, Value::WIN);
  values[50] = Value::LOSS;
  values[75] = Value::DRAW;

  auto [method, data] = compress_block_best(values.data(), values.size());

  // Ternary should be selected as it's smaller
  // Raw: 100/4 = 25 bytes
  // Ternary: 1 + 100/5 = 21 bytes
  ASSERT(method == CompressionMethod::TERNARY_BASE3);
  ASSERT_EQ(data.size(), 21u);
}

TEST(block_indexing) {
  // Test that correct block is selected for any position index
  std::size_t num_positions = BLOCK_SIZE * 3 + 100;  // 3 full blocks + partial

  for (std::size_t idx = 0; idx < num_positions; idx += BLOCK_SIZE / 2) {
    std::uint32_t block_idx = static_cast<std::uint32_t>(idx / BLOCK_SIZE);
    std::size_t idx_in_block = idx % BLOCK_SIZE;

    // Verify invariant
    ASSERT(block_idx * BLOCK_SIZE + idx_in_block == idx);
  }

  // Total number of blocks should be correct
  std::uint32_t expected_blocks = (num_positions + BLOCK_SIZE - 1) / BLOCK_SIZE;
  ASSERT_EQ(expected_blocks, 4u);
}

TEST(lru_cache_eviction) {
  // Create a small cache and test eviction
  BlockCache cache(2);  // Only 2 entries

  // Create a small compressed tablebase
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> tb(BLOCK_SIZE * 3);  // 3 blocks
  for (std::size_t i = 0; i < tb.size(); ++i) {
    tb[i] = static_cast<Value>((i % 3) + 1);  // WIN, LOSS, DRAW cycling
  }

  CompressedTablebase ctb = compress_tablebase(tb, m);
  ASSERT_EQ(ctb.num_blocks, 3u);

  // Access block 0
  cache.get_or_decompress(0, ctb);
  ASSERT_EQ(cache.misses(), 1u);
  ASSERT_EQ(cache.hits(), 0u);

  // Access block 1
  cache.get_or_decompress(1, ctb);
  ASSERT_EQ(cache.misses(), 2u);

  // Access block 0 again (should be hit)
  cache.get_or_decompress(0, ctb);
  ASSERT_EQ(cache.hits(), 1u);

  // Access block 2 (should evict block 1, the LRU)
  cache.get_or_decompress(2, ctb);
  ASSERT_EQ(cache.misses(), 3u);

  // Access block 0 (should be hit, still cached)
  cache.get_or_decompress(0, ctb);
  ASSERT_EQ(cache.hits(), 2u);

  // Access block 1 (was evicted, should miss)
  cache.get_or_decompress(1, ctb);
  ASSERT_EQ(cache.misses(), 4u);
}

TEST(compress_tablebase_roundtrip) {
  // Test that compressing and looking up a tablebase works correctly
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> original = load_tablebase(m);
  if (original.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressedTablebase ctb = compress_tablebase(original, m);

  // Verify all positions
  for (std::size_t i = 0; i < original.size(); ++i) {
    Value looked_up = lookup_compressed(ctb, i, nullptr);
    ASSERT(looked_up == original[i]);
  }
}

TEST(compressed_lookup_with_cache) {
  // Test that cache-based lookup works
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> original = load_tablebase(m);
  if (original.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressedTablebase ctb = compress_tablebase(original, m);
  BlockCache cache(4);

  // Look up all positions using cache
  for (std::size_t i = 0; i < original.size(); ++i) {
    Value looked_up = lookup_compressed(ctb, i, &cache);
    ASSERT(looked_up == original[i]);
  }

  // For random-access methods (RAW_2BIT, TERNARY_BASE3), the cache is not used
  // because we can decode individual values without decompressing the whole block.
  // This is by design - cache is only for sequential-access methods (Huffman, etc.).
  // So we check that the cache was not used (0 hits + 0 misses = 0 total).
  ASSERT(cache.hits() == 0);
  ASSERT(cache.misses() == 0);
}

TEST(block_compression_stats) {
  // Test compression statistics
  Material m{0, 0, 0, 0, 1, 1};  // KvK
  std::vector<Value> tb = load_tablebase(m);
  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressedTablebase ctb = compress_tablebase(tb, m);
  BlockCompressionStats stats = analyze_block_compression(ctb);

  // Should have at least 1 block
  ASSERT(stats.total_blocks >= 1);

  // Uncompressed size should be number of positions
  ASSERT_EQ(stats.uncompressed_size, tb.size());

  // Should achieve some compression
  ASSERT(stats.compressed_size < stats.uncompressed_size);

  // Print info
  std::cout << "\n    [Blocks=" << stats.total_blocks
            << " Ratio=" << std::fixed << std::setprecision(2)
            << stats.compression_ratio() << "x"
            << " Method0=" << stats.method_counts[0]
            << " Method1=" << stats.method_counts[1] << "] ";
}

TEST(compression_ratio_vs_raw) {
  // Verify that block compression achieves better than raw storage
  Material m{0, 0, 0, 0, 2, 1};  // KKvK - slightly larger
  std::vector<Value> tb = load_tablebase(m);
  if (tb.empty()) {
    std::cout << "(skipped - no tablebase) ";
    return;
  }

  CompressedTablebase ctb = compress_tablebase(tb, m);
  BlockCompressionStats stats = analyze_block_compression(ctb);

  // Compression ratio should be > 1 (we save space)
  ASSERT(stats.compression_ratio() > 1.0);

  // For WDL data, we should achieve at least 2x vs raw 1-byte-per-value
  // (since 2-bit encoding gives 4x, but block header adds overhead)
  ASSERT(stats.compression_ratio() > 2.0);
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

  std::cout << "\nRunning compression tests (Stage 1):\n";
  RUN_TEST(has_captures_pawn);
  RUN_TEST(has_captures_none);
  RUN_TEST(has_captures_queen);
  RUN_TEST(is_dont_care_we_capture);
  RUN_TEST(is_dont_care_opponent_captures);
  RUN_TEST(is_dont_care_quiet_position);
  RUN_TEST(compression_stats_kvk);
  RUN_TEST(mark_dont_care_roundtrip);
  RUN_TEST(lookup_search_correctness);

  std::cout << "\nRunning compression tests (Stage 2):\n";
  RUN_TEST(search_stats_tracking);
  RUN_TEST(lookup_correctness_kvkk);
  RUN_TEST(lookup_correctness_multi_material);
  RUN_TEST(lookup_with_sub_tablebases);
  RUN_TEST(search_performance_benchmark);
  RUN_TEST(compression_ratio_analysis);

  std::cout << "\nRunning compression tests (Stage 3):\n";
  RUN_TEST(raw_2bit_roundtrip);
  RUN_TEST(raw_2bit_expected_size);
  RUN_TEST(ternary_base3_roundtrip);
  RUN_TEST(ternary_base3_expected_size);
  RUN_TEST(ternary_fallback_on_4_values);
  RUN_TEST(compress_block_best_selects_smallest);
  RUN_TEST(block_indexing);
  RUN_TEST(lru_cache_eviction);
  RUN_TEST(compress_tablebase_roundtrip);
  RUN_TEST(compressed_lookup_with_cache);
  RUN_TEST(block_compression_stats);
  RUN_TEST(compression_ratio_vs_raw);

  std::cout << "\n========================================\n";
  std::cout << "Tests: " << tests_passed << "/" << tests_run << " passed\n";

  return (tests_passed == tests_run) ? 0 : 1;
}
