# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Spanish Checkers (Damas) game engine with endgame tablebase generation and analysis. Written in C++20 with bitboard representation and OpenMP parallelization.

## Build Commands

```bash
make all       # Build all executables: damas, test, generate, lookup, verify
make damas     # Main interactive game engine
make test      # Test suite (circular captures, notation, perft)
make generate  # Tablebase generator (OpenMP parallelized)
make lookup    # Tablebase lookup utility
make verify    # Tablebase verification utility
make clean     # Remove compiled objects and binaries
```

Compiler: GCC with C++20, O3 optimization, BMI2 instructions, OpenMP.

## Architecture

### Board Representation (board.h/cpp)
- Uses 32-bit bitboards (`Bb = uint32_t`) for the 32 playable light squares
- Three bitboards: white pieces, black pieces, kings
- **Key convention:** Board flips after each move so white is always to move
- Internal indexing: 0-31; notation indexing: 1-32

### Move Generation (movegen.h/cpp)
- Template-based collectors for compact moves vs full moves with path info
- Handles pawn movement, queen movement, multi-capture sequences
- Mandatory captures with longest capture rule
- Circular captures: pieces capturing all adjacent enemies returning to same square (`from_xor_to == 0`)

### Notation System (notation.h/cpp)
- Move strings: "9-13" (simple move), "9x14x23" (capture path)
- Game record parsing and serialization

### Tablebase System (tablebase.h/cpp, generate.cpp)
- Material configuration encodes piece distribution
- **WDL encoding:** 2-bit per position (Win/Draw/Loss)
- **DTM encoding:** 16-bit signed integer (Distance-To-Mate)
- Gapless indexing using combinatorial number system with BMI2 `_pext_u32`/`_pdep_u32`
- File format: `dtm_BBWWKK.bin` (black pieces, white pieces, black kings, white kings)
- Retrograde analysis from terminal positions
- Thread-safe parallel generation with OpenMP and `std::mutex`

### DTM Value Conventions
- Positive (1-127): WIN in M moves
- Zero: DRAW
- Negative (-1 to -127): LOSS in M moves
- -128: Terminal LOSS (captured)
- -32768: UNKNOWN position

## Key Files

| File | Purpose |
|------|---------|
| board.h/cpp | Board structure, bitboards, move application |
| movegen.h/cpp | Legal move generation with template collectors |
| notation.h/cpp | Move/game string parsing and serialization |
| tablebase.h/cpp | Indexing, combinatorics, file I/O |
| generate.cpp | Parallel tablebase generation engine |
| verify.cpp | Tablebase consistency verification |
| test.cpp | Test suite with perft benchmarking |
| main.cpp | Interactive game entry point |

## Running Tests

```bash
make test
./test
```

Tests include: circular capture validation, notation round-trip verification, perft node counting (depths 1-10) with performance benchmarking.

## Requirements

- GCC with C++20 support
- x86-64 processor with BMI2 support
- OpenMP development libraries
- Linux
