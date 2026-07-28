/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_GuardDescriptorCollector_h
#define jit_GuardDescriptorCollector_h

#ifdef JS_GUARD_DESCRIPTORS

#  include "vm/Runtime.h"

namespace js {
namespace jit {
void collectGuardsDescriptors(JSRuntime* runtime);
}  // namespace jit
}  // namespace js
#endif

#endif /* jit_GuardDescriptorCollector_h */
