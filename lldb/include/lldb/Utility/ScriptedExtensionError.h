//===-- ScriptedExtensionError.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_UTILITY_SCRIPTEDEXTENSIONERROR_H
#define LLDB_UTILITY_SCRIPTEDEXTENSIONERROR_H

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

namespace lldb_private {

/// "Calling into a scripted extension failed."
///
/// Raised for a script that is *broken* - a method that threw, an object that
/// isn't callable - as opposed to a script that simply answered "no". That
/// distinction matters because many scripted callbacks report absence with the
/// same value they'd otherwise report failure with: a synthetic child provider
/// has no child of a given name, and one whose `get_child_index` threw also
/// can't produce an index, but only the first may be recovered from.
///
/// Callers that recover from a negative answer - by falling back to automatic
/// "[N]" subscripting, say, or by treating a missing child as absent - must key
/// off the *absence*, and let this type through.
class ScriptedExtensionError
    : public llvm::ErrorInfo<ScriptedExtensionError> {
  std::string m_message;

public:
  static char ID;

  explicit ScriptedExtensionError(std::string message)
      : m_message(std::move(message)) {}

  void log(llvm::raw_ostream &OS) const override { OS << m_message; }

  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};
} // namespace lldb_private

#endif // LLDB_UTILITY_SCRIPTEDEXTENSIONERROR_H
