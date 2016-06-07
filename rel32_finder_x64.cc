// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "courgette/rel32_finder_x64.h"

namespace courgette {

Rel32FinderX64::Rel32FinderX64(RVA relocs_start_rva,
                               RVA relocs_end_rva,
                               RVA size_of_image)
    : Rel32Finder(relocs_start_rva, relocs_end_rva),
      size_of_image_(size_of_image) {}

// Scan for opcodes matching the following instructions :
//  rel32 JMP/CALL
//  rip MOV/LEA
//  Jcc (excluding JPO/JPE)
// Falsely detected rel32 that collide with known abs32 or that point outside
// valid regions are discarded.
void Rel32FinderX64::Find(const uint8_t* start_pointer,
                          const uint8_t* end_pointer,
                          RVA start_rva,
                          RVA end_rva,
                          const std::vector<RVA>& abs32_locations) {
  // Quick way to convert from Pointer to RVA within a single Section is to
  // subtract |adjust_pointer_to_rva|.
  const uint8_t* const adjust_pointer_to_rva = start_pointer - start_rva;

  std::vector<RVA>::const_iterator abs32_pos = abs32_locations.begin();

  // Find the rel32 relocations.
  const uint8_t* p = start_pointer;
  while (p < end_pointer) {
    RVA current_rva = static_cast<RVA>(p - adjust_pointer_to_rva);

    // Skip the base reloation table if we encounter it.
    // Note: We're not bothering to handle the edge case where a Rel32 pointer
    // collides with |relocs_start_rva_| by being {1, 2, 3}-bytes before it.
    if (current_rva >= relocs_start_rva_ && current_rva < relocs_end_rva_) {
      p += relocs_end_rva_ - current_rva;
      continue;
    }

    // Heuristic discovery of rel32 locations in instruction stream: are the
    // next few bytes the start of an instruction containing a rel32
    // addressing mode?
    const uint8_t* rel32 = nullptr;
    bool is_rip_relative = false;

    if (p + 5 <= end_pointer) {
      if (p[0] == 0xE8 || p[0] == 0xE9)  // jmp rel32 and call rel32
        rel32 = p + 1;
    }
    if (p + 6 <= end_pointer) {
      if (p[0] == 0x0F && (p[1] & 0xF0) == 0x80) {  // Jcc long form
        if (p[1] != 0x8A && p[1] != 0x8B)           // JPE/JPO unlikely
          rel32 = p + 2;
      } else if (p[0] == 0xFF && (p[1] == 0x15 || p[1] == 0x25)) {
        // rip relative CALL/JMP
        rel32 = p + 2;
        is_rip_relative = true;
      }
    }
    // TODO(etiennep): Many rip mov/lea variants are not detected. Experiment,
    // fix and combine logic.
    if (p + 7 <= end_pointer) {
      if ((p[0] & 0xFB) == 0x48 &&  // Dst reg : 48/4C [rax-rdi]/[r8-r15]
          p[1] == 0x8D &&           // LEA
          (p[2] & 0xC7) == 0x05) {  // Dst reg : [05,0D,...3D] =
                                    //  [rax,rbx,...,rdi]/[r8,r9,...,r15]
        // LEA dst, QWORD [rip + rel32]
        rel32 = p + 3;
        is_rip_relative = true;
      } else if ((p[0] & 0xFB) == 0x48 &&  // Dst reg : 48/4C [rax-rdi]/[r8-r15]
                 p[1] == 0x8B &&           // MOV
                 (p[2] & 0xC7) == 0x05) {  // Dst reg : [05,0D,...3D] =
                                           //  [rax,rbx,...,rdi]/[r8,r9,...,r15]
        // MOV dst, QWORD PTR[rip + rel32]
        rel32 = p + 3;
        is_rip_relative = true;
      }
    }

    if (rel32) {
      RVA rel32_rva = static_cast<RVA>(rel32 - adjust_pointer_to_rva);

      // Is there an abs32 reloc overlapping the candidate?
      while (abs32_pos != abs32_locations.end() && *abs32_pos < rel32_rva - 3)
        ++abs32_pos;
      // Now: (*abs32_pos > rel32_rva - 4) i.e. the lowest addressed 4-byte
      // region that could overlap rel32_rva.
      if (abs32_pos != abs32_locations.end()) {
        if (*abs32_pos < rel32_rva + 4) {
          // Beginning of abs32 reloc is before end of rel32 reloc so they
          // overlap. Skip four bytes past the abs32 reloc.
          p += (*abs32_pos + 4) - current_rva;
          continue;
        }
      }

      // + 4 since offset is relative to start of next instruction.
      RVA target_rva = rel32_rva + 4 + Read32LittleEndian(rel32);
      // To be valid, rel32 target must be within image, and within this
      // section.
      if (target_rva < size_of_image_ &&  // Subsumes rva != kUnassignedRVA.
          (is_rip_relative ||
           (start_rva <= target_rva && target_rva < end_rva))) {
        rel32_locations_.push_back(rel32_rva);
#if COURGETTE_HISTOGRAM_TARGETS
        ++rel32_target_rvas_[target_rva];
#endif
        p = rel32 + 4;
        continue;
      }
    }
    p += 1;
  }
}

}  // namespace courgette
