//===-- llvm/Bitcode/ReaderWriter.h - Bitcode reader/writers ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This header defines interfaces to read and write LLVM bitcode files/streams.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BITCODE_H
#define LLVM_BITCODE_H

#include <string>

namespace llvm {
  class MemoryBuffer;
  class DataStreamer;
  class LLVMContext;
  class Module;
  class ModulePass;
  class raw_ostream;

namespace ver_3_1 {
  class BitstreamWriter;

  /// WriteBitcodeToFile - Write the specified module to the specified
  /// raw output stream.  For streams where it matters, the given stream
  /// should be in "binary" mode.
  void WriteBitcodeToFile(const Module *M, raw_ostream &Out);
} // End ver_3_1 namespace
} // End llvm namespace

#endif
