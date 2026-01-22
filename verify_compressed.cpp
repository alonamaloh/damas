#include "tablebase.h"
#include "compression.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <filesystem>

namespace {

// Global manager instance (initialized in main)
CompressedTablebaseManager* g_manager = nullptr;

// Wrapper function for lookup
Value lookup_wdl(const Board& b) {
  return g_manager->lookup_wdl(b);
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

// Verify a single position's WDL value
// Returns true if consistent, false if error found
bool verify_position(const Board& board, Value stored_wdl,
                     std::size_t idx, bool verbose) {
  std::vector<Move> moves;
  generateMoves(board, moves);

  // Terminal position check (no legal moves)
  if (moves.empty()) {
    if (stored_wdl != Value::LOSS) {
      if (verbose) {
        std::cout << "  ERROR at index " << idx << " (terminal position):" << std::endl;
        print_board(board);
        std::cout << "    Expected: LOSS" << std::endl;
        std::cout << "    Got: " << value_to_string(stored_wdl) << std::endl;
      }
      return false;
    }
    return true;
  }

  // Collect successor values
  bool found_loss = false;
  bool all_wins = true;

  for (const Move& move : moves) {
    Board next = makeMove(board, move);

    // Terminal: opponent has no pieces after our move
    Material next_m = get_material(next);
    Value succ_wdl;
    if (next_m.white_pieces() == 0) {
      succ_wdl = Value::LOSS;  // Opponent lost
    } else {
      succ_wdl = lookup_wdl(next);
    }

    if (succ_wdl == Value::LOSS) {
      found_loss = true;
    }
    if (succ_wdl != Value::WIN) {
      all_wins = false;
    }
  }

  // Compute expected WDL from successors
  Value computed_wdl;
  if (found_loss) {
    computed_wdl = Value::WIN;
  } else if (all_wins) {
    computed_wdl = Value::LOSS;
  } else {
    computed_wdl = Value::DRAW;
  }

  if (computed_wdl != stored_wdl) {
    if (verbose) {
      std::cout << "  WDL ERROR at index " << idx << ":" << std::endl;
      print_board(board);
      std::cout << "    Stored: " << value_to_string(stored_wdl) << std::endl;
      std::cout << "    Computed: " << value_to_string(computed_wdl) << std::endl;
      std::cout << "    Successors:" << std::endl;
      for (const Move& move : moves) {
        Board next = makeMove(board, move);
        Material nm = get_material(next);
        Value sv = (nm.white_pieces() == 0) ? Value::LOSS : lookup_wdl(next);
        std::cout << "      -> " << value_to_string(sv) << " (mat=" << nm << ")" << std::endl;
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
    // Enumerate all ways to distribute pieces
    // Note: bounds allow zero queens (all pieces can be pawns)
    for (int bwp = 0; bwp <= std::min(4, total); ++bwp) {
      for (int bbp = 0; bbp <= std::min(4, total - bwp); ++bbp) {
        for (int owp = 0; owp <= std::min(24, total - bwp - bbp); ++owp) {
          for (int obp = 0; obp <= std::min(24 - owp, total - bwp - bbp - owp); ++obp) {
            int remaining = total - bwp - bbp - owp - obp;
            for (int wq = 0; wq <= remaining; ++wq) {
              int bq = remaining - wq;
              if (bq >= 0) {
                Material m{bwp, bbp, owp, obp, wq, bq};
                // Must have at least one piece on each side
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
  std::cerr << "Usage: " << program << " [options] <directory> <max_pieces>" << std::endl;
  std::cerr << std::endl;
  std::cerr << "Options:" << std::endl;
  std::cerr << "  -v, --verbose    Show detailed error information" << std::endl;
  std::cerr << std::endl;
  std::cerr << "Verifies all compressed WDL tablebases (cwdl_*.bin) up to max_pieces (2-8)." << std::endl;
  std::cerr << "Checks that each position's value is consistent with its successors." << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
  bool verbose = false;
  std::string directory = ".";
  int max_pieces = 0;

  // Parse arguments
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
    } else if (argv[i][0] != '-') {
      positional.push_back(argv[i]);
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  if (positional.size() == 1) {
    // Just max_pieces, use current directory
    max_pieces = std::atoi(positional[0].c_str());
  } else if (positional.size() == 2) {
    directory = positional[0];
    max_pieces = std::atoi(positional[1].c_str());
  } else {
    print_usage(argv[0]);
    return 1;
  }

  if (max_pieces < 2 || max_pieces > 8) {
    print_usage(argv[0]);
    return 1;
  }

  std::cout << "=== Compressed Tablebase Verification ===" << std::endl;
  std::cout << "Directory: " << directory << std::endl;
  std::cout << "Verifying up to " << max_pieces << " pieces" << std::endl;
  std::cout << std::endl;

  // Initialize the global manager
  CompressedTablebaseManager manager(directory);
  g_manager = &manager;

  std::vector<Material> materials = all_materials(max_pieces);

  std::size_t total_materials = 0;
  std::size_t total_positions = 0;
  std::size_t total_errors = 0;
  std::size_t skipped_materials = 0;

  for (const Material& m : materials) {
    // Try to load compressed tablebase
    const CompressedTablebase* tb = manager.get_tablebase(m);
    if (!tb) {
      skipped_materials++;
      continue;
    }

    std::size_t size = tb->num_positions;
    std::cout << "Verifying " << m << " (" << size << " positions)..." << std::flush;

    std::size_t errors = 0;

    for (std::size_t idx = 0; idx < size; ++idx) {
      Board board = index_to_board(idx, m);

      // Get the stored value (with search for tense positions)
      Value stored_wdl = lookup_wdl(board);

      if (!verify_position(board, stored_wdl, idx, verbose)) {
        errors++;
        if (!verbose && errors >= 10) {
          // Stop after 10 errors in non-verbose mode
          std::cout << " (stopping after 10 errors)" << std::endl;
          break;
        }
      }
    }

    if (errors == 0) {
      std::cout << " OK" << std::endl;
    } else {
      std::cout << " " << errors << " ERRORS" << std::endl;
    }

    total_materials++;
    total_positions += size;
    total_errors += errors;
  }

  std::cout << std::endl;
  std::cout << "=== Verification Summary ===" << std::endl;
  std::cout << "Materials verified: " << total_materials << std::endl;
  std::cout << "Materials skipped: " << skipped_materials << std::endl;
  std::cout << "Positions checked: " << total_positions << std::endl;
  std::cout << "Errors: " << total_errors << std::endl;

  if (total_errors == 0) {
    std::cout << std::endl << "All compressed tablebases verified successfully!" << std::endl;
    return 0;
  } else {
    std::cout << std::endl << "VERIFICATION FAILED" << std::endl;
    return 1;
  }
}
