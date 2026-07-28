/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_GUARD_DESCRIPTORS
#  include "jit/GuardDescriptorCollector.h"

#  include "gc/PublicIterators.h"
#  include "jit/BaselineIC.h"
#  include "jit/CacheIR.h"
#  include "jit/CacheIRCompiler.h"
#  include "jit/CacheIRReader.h"
#  include "jit/CacheIRSpewer.h"
#  include "jit/JitScript.h"
#  include "jit/JitZone.h"
#  include "js/Printer.h"
#  include "vm/JSScript.h"

using namespace js;
using namespace js::jit;

void js::jit::collectGuardsDescriptors(JSRuntime* runtime) {
  SEprinter printer = SEprinter();

  printer.printf("Iterating zones...\n");
  for (ZonesIter zone(runtime, js::SkipAtoms); !zone.done(); zone.next()) {
    printer.printf("\n Got Zone!\n");
    jit::JitZone* jitZone = zone.get()->jitZone();
    if (!jitZone) {
      continue;
    }
    printer.printf(" Iterating scripts...\n");
    jitZone->forEachJitScript([&](jit::JitScript* jitScript) {
      printer.printf("\n  Got JitScript!\n");
      ICScript* icScript = jitScript->icScript();
      if (!icScript) {
        return;
      }
      JSScript* script = jitScript->owningScript();
      printer.printf("  Iterating IC Entries...\n");
      for (uint32_t i = 0; i < icScript->numICEntries(); i++) {
        printer.printf("   Got Entry!\n");
        jit::ICEntry& entry = icScript->icEntry(i);
        jit::ICFallbackStub* fallback = icScript->fallbackStub(i);

        jsbytecode* pc = script->offsetToPC(fallback->pcOffset());
        JS::LimitedColumnNumberOneOrigin column;
        unsigned line = PCToLineNumber(script, pc, &column);

        printer.printf("   Entry @ %s:%u:%u (%s)\n", script->filename(), line,
                       column.oneOriginValue(), CodeName(JSOp(*pc)));

        jit::ICStub* stub = entry.firstStub();
        printer.printf("   Iterating stubs...\n");
        while (!stub->isFallback()) {
          printer.printf("    Got Stub!\n");
          ICCacheIRStub* cacheIRStub = stub->toCacheIRStub();
          const CacheIRStubInfo* info = cacheIRStub->stubInfo();
          SpewCacheIROps(printer, "", info);
          // do {
          //   CacheOp op = reader.readOp();

          //   printer.printf("    > %s\n", CacheIRCodeName(op));
          //   uint32_t argLength = CacheIROpInfos[size_t(op)].argLength;
          //   reader.skip(argLength);
          // } while (reader.more());
          stub = cacheIRStub->next();
        }
      }
    });
  }
}

#endif /* JS_GUARD_DESCRIPTORS */
