#include "tablebase.h"
#include "board.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>

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

// Convert material to a human-readable string like "KK" or "KPK" or "KPPKP"
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
  std::cout << "  Position: ";

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

int main(int argc, char* argv[]) {
  int max_pieces = 6;
  if (argc > 1) {
    max_pieces = std::atoi(argv[1]);
    if (max_pieces < 2 || max_pieces > 8) {
      std::cerr << "Max pieces must be between 2 and 8" << std::endl;
      return 1;
    }
  }

  std::cout << "Finding longest mates for all " << max_pieces << "-men tablebases" << std::endl;
  std::cout << "========================================" << std::endl << std::endl;

  std::vector<Material> all = all_materials(max_pieces);
  std::vector<LongestMate> results;

  for (const Material& m : all) {
    // Skip if no DTM tablebase exists
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
      results.push_back({m, max_idx, max_dtm, dtm_to_plies(max_dtm)});
    }
  }

  // Sort by longest mate first
  std::sort(results.begin(), results.end(), [](const LongestMate& a, const LongestMate& b) {
    return a.plies > b.plies;
  });

  // Print results
  std::cout << "Results (sorted by longest mate):" << std::endl;
  std::cout << std::string(60, '-') << std::endl;

  for (const LongestMate& lm : results) {
    Board b = index_to_board(lm.index, lm.material);

    std::cout << std::left << std::setw(12) << material_string(lm.material)
              << " DTM=" << std::setw(4) << lm.dtm
              << " (" << std::setw(3) << lm.plies << " plies)"
              << std::endl;
    print_position(b);
    std::cout << b << std::endl;
  }

  std::cout << std::string(60, '-') << std::endl;
  std::cout << "Total material configurations with wins: " << results.size() << std::endl;

  if (!results.empty()) {
    std::cout << "\nOverall longest mate: " << material_string(results[0].material)
              << " with " << results[0].plies << " plies" << std::endl;
  }

  return 0;
}
