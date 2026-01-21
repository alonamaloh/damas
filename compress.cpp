#include "tablebase.h"
#include "compression.h"
#include "movegen.h"
#include <iostream>
#include <iomanip>
#include <filesystem>

// Compress all WDL tablebase files in the current directory (or specified directory).
// For each tb_BBWWKK.bin file, creates a compressed cwdl_BBWWKK.bin file.

int main(int argc, char* argv[]) {
  std::string directory = ".";
  if (argc > 1) {
    directory = argv[1];
  }

  std::cout << "Compressing WDL tablebases in: " << directory << "\n\n";

  std::size_t total_original = 0;
  std::size_t total_compressed = 0;
  int files_processed = 0;
  int files_skipped = 0;
  int files_error = 0;

  // Method usage statistics
  std::size_t method_counts[16] = {0};
  std::size_t total_blocks = 0;

  // Iterate all tablebase files
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;

    std::string filename = entry.path().filename().string();
    if (filename.size() < 10 || filename.substr(0, 3) != "tb_" ||
        filename.substr(filename.size() - 4) != ".bin") {
      continue;
    }

    // Parse material from filename: tb_BBWWKK.bin
    std::string mat_str = filename.substr(3, 6);
    if (mat_str.size() != 6) continue;

    Material m;
    m.back_white_pawns = mat_str[0] - '0';
    m.back_black_pawns = mat_str[1] - '0';
    m.other_white_pawns = mat_str[2] - '0';
    m.other_black_pawns = mat_str[3] - '0';
    m.white_queens = mat_str[4] - '0';
    m.black_queens = mat_str[5] - '0';

    // Load original tablebase
    std::vector<Value> tb = load_tablebase(m);
    if (tb.empty()) {
      files_skipped++;
      continue;
    }

    std::size_t original_size = tb.size();  // 1 byte per value in uncompressed format

    // Compress
    CompressedTablebase ctb = compress_tablebase(tb, m);

    // Calculate compressed size (header + offsets + block data)
    std::size_t header_size = 4 + 1 + 6 + 4 + 4;  // magic + version + material + num_positions + num_blocks
    std::size_t offsets_size = ctb.num_blocks * 4;
    std::size_t compressed_size = header_size + offsets_size + ctb.block_data.size();

    // Generate output filename: cwdl_BBWWKK.bin
    std::string output_filename = directory + "/cwdl_" + mat_str + ".bin";

    // Save compressed tablebase
    if (!save_compressed_tablebase(ctb, output_filename)) {
      std::cerr << "Error saving: " << output_filename << "\n";
      files_error++;
      continue;
    }

    // Collect statistics
    BlockCompressionStats stats = analyze_block_compression(ctb);
    for (int i = 0; i < 16; ++i) {
      method_counts[i] += stats.method_counts[i];
    }
    total_blocks += stats.total_blocks;

    total_original += original_size;
    total_compressed += compressed_size;
    files_processed++;

    // Progress output
    double ratio = static_cast<double>(original_size) / compressed_size;
    std::cout << std::setw(20) << filename << " -> " << std::setw(24) << output_filename
              << "  " << std::setw(10) << original_size << " -> " << std::setw(10) << compressed_size
              << "  (" << std::fixed << std::setprecision(2) << ratio << "x)\n";
  }

  // Summary
  std::cout << "\n========================================\n";
  std::cout << "Compression Summary\n";
  std::cout << "========================================\n";
  std::cout << "Files processed: " << files_processed << "\n";
  std::cout << "Files skipped:   " << files_skipped << "\n";
  std::cout << "Files with errors: " << files_error << "\n";
  std::cout << "\n";

  if (files_processed > 0) {
    double overall_ratio = static_cast<double>(total_original) / total_compressed;
    std::cout << "Total original:   " << std::setw(12) << total_original << " bytes\n";
    std::cout << "Total compressed: " << std::setw(12) << total_compressed << " bytes\n";
    std::cout << "Overall ratio:    " << std::fixed << std::setprecision(2) << overall_ratio << "x\n";
    std::cout << "\n";

    std::cout << "Compression method usage (blocks):\n";
    const char* method_names[] = {
      "RAW_2BIT", "TERNARY_BASE3", "DEFAULT_EXCEPTIONS", "RLE_BINARY_SEARCH",
      "HUFFMAN_RLE_SHORT", "HUFFMAN_RLE_MEDIUM", "HUFFMAN_RLE_LONG", "HUFFMAN_RLE_VARIABLE",
      "RLE_HUFFMAN_2VAL"
    };
    for (int i = 0; i < 9; ++i) {
      if (method_counts[i] > 0) {
        double pct = 100.0 * method_counts[i] / total_blocks;
        std::cout << "  " << std::setw(20) << method_names[i] << ": "
                  << std::setw(8) << method_counts[i] << " ("
                  << std::fixed << std::setprecision(1) << pct << "%)\n";
      }
    }
  }

  return (files_error > 0) ? 1 : 0;
}
