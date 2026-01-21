#pragma once

#include "board.h"
#include "tablebase.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// ============================================================================
// Don't-Care Position Detection (Stage 1)
// ============================================================================

// A position is "don't care" if:
// 1. It has captures available (forced move), OR
// 2. The opponent would have captures after any quiet move we make
//
// For such positions, we don't need to store the WDL value - we can compute
// it by searching through the forced sequence until we reach a "real" position.
bool is_dont_care(const Board& b);

// ============================================================================
// Statistics for Compression Analysis
// ============================================================================

struct CompressionStats {
  std::size_t total_positions = 0;
  std::size_t dont_care_positions = 0;      // We have captures
  std::size_t opponent_capture_positions = 0; // Opponent would have captures
  std::size_t real_positions = 0;           // Need actual storage
  std::size_t wins = 0;
  std::size_t losses = 0;
  std::size_t draws = 0;

  double dont_care_ratio() const {
    return total_positions > 0
      ? static_cast<double>(dont_care_positions + opponent_capture_positions) / total_positions
      : 0.0;
  }

  double compression_potential() const {
    return total_positions > 0
      ? static_cast<double>(real_positions) / total_positions
      : 1.0;
  }
};

// Analyze a tablebase and compute compression statistics
CompressionStats analyze_compression(const std::vector<Value>& tablebase, const Material& m);

// ============================================================================
// WDL Lookup with Search for Don't-Care Positions (Stage 2)
// ============================================================================

// Sentinel value for don't-care positions in compressed storage
constexpr std::uint8_t DONT_CARE = 0;  // Placeholder in compressed format

// Search performance statistics
struct SearchStats {
  std::size_t lookups = 0;           // Total lookup calls
  std::size_t direct_hits = 0;       // Positions resolved without search
  std::size_t searches = 0;          // Positions requiring search
  std::size_t total_nodes = 0;       // Total nodes visited in all searches
  std::size_t max_depth = 0;         // Maximum search depth encountered
  std::size_t cycles_detected = 0;   // Repetitions detected (draws)
  std::size_t terminal_wins = 0;     // Terminal positions (opponent has no pieces)
  std::size_t sub_tb_lookups = 0;    // Lookups into sub-tablebases

  double avg_nodes_per_search() const {
    return searches > 0 ? static_cast<double>(total_nodes) / searches : 0.0;
  }

  double search_ratio() const {
    return lookups > 0 ? static_cast<double>(searches) / lookups : 0.0;
  }
};

// Lookup WDL value, searching through don't-care positions if needed.
// Uses cycle detection to handle repetitions (which are draws).
//
// Parameters:
//   b: The board position to look up
//   tablebase: The compressed tablebase (with DONT_CARE markers)
//   m: The material configuration
//
// Returns the WDL value (WIN, LOSS, or DRAW)
Value lookup_wdl_with_search(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m);

// Version with sub-tablebase support and statistics tracking
// sub_tablebases: Map from material to compressed tablebase for sub-endgames
// stats: Optional output for search statistics (pass nullptr to disable)
Value lookup_wdl_with_search(
    const Board& b,
    const std::vector<Value>& tablebase,
    const Material& m,
    const std::unordered_map<Material, std::vector<Value>>& sub_tablebases,
    SearchStats* stats);

// ============================================================================
// Compressed WDL Generation (Stage 1)
// ============================================================================

// Generate a WDL tablebase with don't-care positions marked.
// Don't-care positions are marked with the DONT_CARE sentinel.
//
// Parameters:
//   original: The original (uncompressed) WDL tablebase
//   m: The material configuration
//   stats: Output statistics about the compression
//
// Returns a new vector with don't-care positions marked
std::vector<Value> mark_dont_care_positions(
    const std::vector<Value>& original,
    const Material& m,
    CompressionStats& stats);

// ============================================================================
// Block-Based Compression (Stage 3+)
// ============================================================================

// Block size for compression (16384 positions per block)
constexpr std::size_t BLOCK_SIZE = 16384;

// Compression method identifiers
enum class CompressionMethod : std::uint8_t {
  RAW_2BIT = 0,           // 2 bits per value, 4 per byte (baseline)
  TERNARY_BASE3 = 1,      // Base-3 encoding, 5 values per byte
  DEFAULT_EXCEPTIONS = 2, // Default value + sorted exception list
  RLE_BINARY_SEARCH = 3,  // Run-length encoding with binary search
  // Future: Huffman RLE variants (4-7)
};

// Compressed block header
struct CompressedBlock {
  CompressionMethod method;
  std::uint16_t compressed_size;
  std::vector<std::uint8_t> data;
};

// Compressed tablebase structure
struct CompressedTablebase {
  Material material;
  std::uint32_t num_positions;
  std::uint32_t num_blocks;
  std::vector<std::uint32_t> block_offsets;  // Offset to each block's data
  std::vector<std::uint8_t> block_data;      // Concatenated compressed blocks

  // Get number of positions
  std::size_t size() const { return num_positions; }

  // Check if empty
  bool empty() const { return num_positions == 0; }
};

// ============================================================================
// File Format Constants (Stage 5)
// ============================================================================

constexpr char CWDL_MAGIC[4] = {'C', 'W', 'D', 'L'};
constexpr std::uint8_t CWDL_VERSION = 1;

