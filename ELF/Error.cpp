//===- Error.cpp ----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Error.h"
#include "Config.h"
#include "Threads.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#endif

using namespace llvm;

using namespace lld;
using namespace lld::elf;

uint64_t elf::ErrorCount;
raw_ostream *elf::ErrorOS;

// The functions defined in this file can be called from multiple threads,
// but outs() or errs() are not thread-safe. We protect them using a mutex.
static std::mutex Mu;

// Prints "\n" or does nothing, depending on Msg contents of
// the previous call of this function.
static void newline(const Twine &Msg) {
  // True if the previous error message contained "\n".
  // We want to separate multi-line error messages with a newline.
  static bool Flag;

  if (Flag)
    *ErrorOS << "\n";
  Flag = StringRef(Msg.str()).contains('\n');
}

static void print(StringRef S, raw_ostream::Colors C) {
  *ErrorOS << Config->Argv[0] << ": ";
  if (Config->ColorDiagnostics) {
    ErrorOS->changeColor(C, true);
    *ErrorOS << S;
    ErrorOS->resetColor();
  } else {
    *ErrorOS << S;
  }
}

void elf::log(const Twine &Msg) {
  if (Config->Verbose) {
    std::lock_guard<std::mutex> Lock(Mu);
    outs() << Config->Argv[0] << ": " << Msg << "\n";
    outs().flush();
  }
}

void elf::message(const Twine &Msg) {
  std::lock_guard<std::mutex> Lock(Mu);
  outs() << Msg << "\n";
  outs().flush();
}

void elf::warn(const Twine &Msg) {
  if (Config->FatalWarnings) {
    error(Msg);
    return;
  }
  static uint64_t WarningCount = 0;
  std::lock_guard<std::mutex> Lock(Mu);
  newline(Msg);
  if (Config->WarningLimit == 0 || WarningCount < Config->WarningLimit) {
    print("warning: ", raw_ostream::MAGENTA);
    *ErrorOS << Msg << "\n";
  } else if (WarningCount == Config->WarningLimit) {
    print("warning: ", raw_ostream::MAGENTA);
    *ErrorOS << "too many warnings emitted, stopping now"
             << " (use -warning-limit=0 to see all warnings)\n";
  }
  ++WarningCount;
}

void elf::error(const Twine &Msg) {
  std::lock_guard<std::mutex> Lock(Mu);
  newline(Msg);

  if (Config->ErrorLimit == 0 || ErrorCount < Config->ErrorLimit) {
    print("error: ", raw_ostream::RED);
    *ErrorOS << Msg << "\n";
  } else if (ErrorCount == Config->ErrorLimit) {
    print("error: ", raw_ostream::RED);
    *ErrorOS << "too many errors emitted, stopping now"
             << " (use -error-limit=0 to see all errors)\n";
    if (Config->ExitEarly)
      exitLld(1);
  }

  ++ErrorCount;
}

void elf::exitLld(int Val) {
  waitForBackgroundThreads();

  // Dealloc/destroy ManagedStatic variables before calling
  // _exit(). In a non-LTO build, this is a nop. In an LTO
  // build allows us to get the output of -time-passes.
  llvm_shutdown();

  outs().flush();
  errs().flush();
  _exit(Val);
}

void elf::fatal(const Twine &Msg) {
  error(Msg);
  exitLld(1);
}
