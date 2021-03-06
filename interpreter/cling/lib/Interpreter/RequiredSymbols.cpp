//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Axel Naumann <axel@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/CValuePrinter.h"
#include "cling/Interpreter/DynamicExprInfo.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/ValuePrinter.h"
#include "cling/Interpreter/ValuePrinterInfo.h"

#include "clang/AST/Type.h"

#include "llvm/Support/raw_ostream.h"

namespace cling {
namespace internal {
void symbol_requester() {
   const char* const argv[] = {"libcling__symbol_requester", 0};
   Interpreter I(1, argv);
   ValuePrinterInfo VPI(0, 0); // asserts, but we don't call.
   valuePrinterInternal::printValue_Default(0, VPI);
   cling_PrintValue(0, 0, 0);
   valuePrinterInternal::flushToStream(llvm::outs(), "");
   LookupHelper h(0,0);
   h.findType("");
   h.findScope("");
   h.findFunctionProto(0, "", "");
   h.findFunctionArgs(0, "", "");
   runtime::internal::DynamicExprInfo DEI(0,0,false);
   DEI.getExpr();
   InterpreterCallbacks cb(0);
}
}
}
