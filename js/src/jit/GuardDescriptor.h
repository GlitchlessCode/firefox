/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_GuardDescriptor_h
#define jit_GuardDescriptor_h

// [SMDOC] Guard Descriptors
//
// TODO: Write SMDOC
//
// SAFETY: All guard descriptors MUST only be used with a system of
// the same build version. Certain values (eg. *FuseIndexImm) use a
// index which may change internally between versions. Additionally,
// CacheIR format can technically change between versions. These,
// and other breaking changes require that any stale guard
// descriptors must be discarded. The safest way to fail here is to
// always discard on version change.
#ifdef JS_GUARD_DESCRIPTORS

#  include <cstdint>
#  include <map>
#  include <unordered_map>

#  include "jit/BaselineIC.h"
#  include "js/Printer.h"

namespace js {
namespace jit {

class GuardDescriptorCollector {
 private:
  static GuardDescriptorCollector guardDescriptorCollector;

  GuardDescriptorCollector() {};
  ~GuardDescriptorCollector() {};

 public:
  enum class ArgClassification : uint8_t;
  enum class ArgKind : uint8_t;

  static GuardDescriptorCollector& singleton() {
    return guardDescriptorCollector;
  }

  void collectStub(ICCacheIRStub* stub);
  void collectStubStats(ICCacheIRStub* stub);

  void dumpStats();
  void dumpStats(GenericPrinter& printer);

 private:
  std::map<ArgClassification, uint32_t> classificationCounters;
  std::map<ArgClassification, uint32_t> opMaxClassificationCounters;
  std::map<ArgKind, uint32_t> argCounters;
  uint32_t totalArgCounter;
  uint32_t totalOpCounter;
};

}  // namespace jit
}  // namespace js

#endif

#endif /* jit_GuardDescriptor_h */
