/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Math.h"
#include "gc/AllocKind.h"
#include "jit/CacheIRWriter.h"
#include "js/ScalarType.h"
#include "vm/RealmFuses.h"
#include "vm/RuntimeFuses.h"
#include "wasm/WasmTypeDecls.h"
#ifdef JS_GUARD_DESCRIPTORS
#  include "jit/GuardDescriptor.h"

#  include <concepts>
#  include <cstdint>
#  include <cstring>
#  include <map>
#  include <type_traits>
#  include <utility>
#  include <vector>

#  include "jit/BaselineIC.h"
#  include "jit/CacheIR.h"
#  include "jit/CacheIRReader.h"
#  include "jit/GuardDescriptorArgKindsGenerated.h"
#  include "js/Printer.h"
#  include "mozilla/Maybe.h"
#  include "mozilla/Span.h"
#  include "vm/JSONPrinter.h"

using namespace js;
using namespace js::jit;

enum class GuardDescriptorCollector::ArgKind : uint8_t {
#  define DEFINE_ARG_KIND(kind) kind,
  GUARD_DESCRIPTOR_ARG_KINDS(DEFINE_ARG_KIND)
#  undef DEFINE_ARG_KIND
};

enum class GuardDescriptorCollector::ArgClassification : uint8_t {
#  define DEFINE_CLASSIFICATION_KIND(kind) kind,
  GUARD_DESCRIPTOR_CLASSIFICATION_KINDS(DEFINE_CLASSIFICATION_KIND)
#  undef DEFINE_CLASSIFICATION_KIND
};

using enum GuardDescriptorCollector::ArgKind;
#  define DEFINE_OP_ARG_KINDS(op, len, ...) \
    constexpr GuardDescriptorCollector::ArgKind op##ArgKinds[] = {__VA_ARGS__};
GUARD_DESCRIPTOR_OP_ARG_KINDS(DEFINE_OP_ARG_KINDS)
#  undef DEFINE_OP_ARG_KINDS

GuardDescriptorCollector::ArgClassification ArgClassificationOf(
    GuardDescriptorCollector::ArgKind arg) {
  switch (arg) {
    using enum GuardDescriptorCollector::ArgClassification;
#  define ARGCLASS(arg, cls) \
    case arg:                \
      return cls;
    GUARD_DESCRIPTOR_ARG_KIND_CLASSIFICATIONS(ARGCLASS)
#  undef ARGCLASS
  }
}

const char* ArgKindName(GuardDescriptorCollector::ArgKind arg) {
  switch (arg) {
#  define ARG_KIND_NAME(kind) \
    case kind:                \
      return #kind;
    GUARD_DESCRIPTOR_ARG_KINDS(ARG_KIND_NAME)
#  undef ARG_KIND_NAME
  }
}

const char* ArgClassificationName(
    GuardDescriptorCollector::ArgClassification classification) {
  using enum GuardDescriptorCollector::ArgClassification;
  switch (classification) {
#  define ARG_CLASS_NAME(kind) \
    case kind:                 \
      return #kind;
    GUARD_DESCRIPTOR_CLASSIFICATION_KINDS(ARG_CLASS_NAME)
#  undef ARG_KIND_NAME
  }
}

struct ArgsList {
  const GuardDescriptorCollector::ArgKind* kinds;
  size_t len;
};

ArgsList ArgsListOf(CacheOp op) {
  switch (op) {
    using enum CacheOp;
#  define OPARGS(op, len, ...) \
    case op:                   \
      return {op##ArgKinds, len};
    GUARD_DESCRIPTOR_OP_ARG_KINDS(OPARGS)
#  undef OPARGS
    default:
      return {{}, 0};  // TODO: Add some failure condition
  }
}

class GuardDescriptorOpWriter;
class GuardDescriptorOpReader;
template <typename C>
concept CacheIRCodec =
    requires(CacheIRReader& cReader, GuardDescriptorOpWriter& gWriter,
             GuardDescriptorOpReader& gReader) {
      typename C::Source;
      { C::Serialize(cReader, gWriter) } -> std::same_as<bool>;
      {
        C::Deserialize(gReader)
      } -> std::same_as<mozilla::Maybe<typename C::Source>>;
    };

class GuardDescriptorOpWriter {
 private:
  friend class GuardDescriptorStubWriter;

  std::vector<uint8_t> bytes;
  bool failed_ = false;

 public:
  GuardDescriptorOpWriter(CacheOp op) {
    bytes.push_back((uint8_t)op);
    bytes.push_back(((uint16_t)op) >> 8);
  }

  void writeBytes(uint8_t val[], size_t len) {
    for (size_t i = 0; i < len; i++) {
      writeUint8(val[i]);
    }
  }
  void writeUint8(uint8_t val) { bytes.push_back(val); }
  void writeUint16(uint16_t val) {
    uint8_t arr[2] = {(uint8_t)val, (uint8_t)(val >> 8)};
    writeBytes(arr, 2);
  }
  void writeUint32(uint32_t val) {
    uint8_t arr[4] = {(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16),
                      (uint8_t)(val >> 24)};
    writeBytes(arr, 4);
  }
  void writeUint64(uint64_t val) {
    uint8_t arr[8] = {(uint8_t)val,         (uint8_t)(val >> 8),
                      (uint8_t)(val >> 16), (uint8_t)(val >> 24),
                      (uint8_t)(val >> 32), (uint8_t)(val >> 40),
                      (uint8_t)(val >> 48), (uint8_t)(val >> 56)};
    writeBytes(arr, 8);
  }

  template <typename T>
  void writeRaw(T val) {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (sizeof(T) == 1) {
      writeUint8(static_cast<uint8_t>(val));
    } else if constexpr (sizeof(T) == 2) {
      writeUint16(static_cast<uint16_t>(val));
    } else if constexpr (sizeof(T) == 4) {
      writeUint32(static_cast<uint32_t>(val));
    } else if constexpr (sizeof(T) == 8) {
      writeUint64(static_cast<uint64_t>(val));
    } else {
      static_assert(sizeof(T) == 0, "Unsupported width for writeRaw");
    }
  }

  // Tries to write an argument with the given codec to the internal buffer.
  // Returns true if it was serialized, false if it failed.
  // An op which fails to have an argument written must NEVER be written out.
  template <typename Codec>
  void writeArg(CacheIRReader& reader)
    requires CacheIRCodec<Codec>
  {
    if (!failed_) {
      failed_ = !Codec::Serialize(reader, *this);
    }
  }

  bool failed() { return failed_; }

  const std::vector<uint8_t>& data() const { return bytes; }
};

class GuardDescriptorOpReader {
 private:
  mozilla::Span<const uint8_t> bytes;
  size_t pos_ = 0;

 public:
  explicit GuardDescriptorOpReader(mozilla::Span<const uint8_t> bytes)
      : bytes(bytes) {}

  mozilla::Maybe<uint8_t> readUint8() {
    if (pos_ + 1 > bytes.Length()) {
      return mozilla::Nothing();
    }
    return mozilla::Some(bytes[pos_++]);
  }
  mozilla::Maybe<uint16_t> readUint16() {
    mozilla::Maybe<uint8_t> lo = readUint8();
    mozilla::Maybe<uint8_t> hi = readUint8();
    if (!lo || !hi) {
      return mozilla::Nothing();
    }
    return mozilla::Some(uint16_t(uint16_t(*lo) | (uint16_t(*hi) << 8)));
  }
  mozilla::Maybe<uint32_t> readUint32() {
    mozilla::Maybe<uint16_t> lo = readUint16();
    mozilla::Maybe<uint16_t> hi = readUint16();
    if (!lo || !hi) {
      return mozilla::Nothing();
    }
    return mozilla::Some(uint32_t(*lo) | (uint32_t(*hi) << 16));
  }
  mozilla::Maybe<uint64_t> readUint64() {
    mozilla::Maybe<uint32_t> lo = readUint32();
    mozilla::Maybe<uint32_t> hi = readUint32();
    if (!lo || !hi) {
      return mozilla::Nothing();
    }
    return mozilla::Some(uint64_t(*lo) | (uint64_t(*hi) << 32));
  }

  template <typename T>
  mozilla::Maybe<T> readRaw() {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (sizeof(T) == 1) {
      return readUint8().map([](uint8_t v) { return static_cast<T>(v); });
    } else if constexpr (sizeof(T) == 2) {
      return readUint16().map([](uint16_t v) { return static_cast<T>(v); });
    } else if constexpr (sizeof(T) == 4) {
      return readUint32().map([](uint32_t v) { return static_cast<T>(v); });
    } else if constexpr (sizeof(T) == 8) {
      return readUint64().map([](uint64_t v) { return static_cast<T>(v); });
    } else {
      static_assert(sizeof(T) == 0, "Unsupported width for readRaw");
    }
  }

  template <typename Codec>
  mozilla::Maybe<typename Codec::Source> read()
    requires CacheIRCodec<Codec>
  {
    return Codec::Deserialize(*this);
  }
};

class GuardDescriptorStubWriter {
 private:
  std::vector<uint8_t> bytes;

 public:
  GuardDescriptorStubWriter() {}

  // SAFETY: `writer` must be both valid and complete
  // - `valid` means that the writer must not have failed() == true
  // - `complete` means that all the arguments for the op must be written
  void writeOp(GuardDescriptorOpWriter& writer) {
    bytes.insert(bytes.end(), writer.bytes.begin(), writer.bytes.end());
  }
};

#  define SIMPLE_CODEC(ty, name, readerMethod)                 \
    struct name##Codec {                                       \
      using Source = ty;                                       \
      static bool Serialize(CacheIRReader& reader,             \
                            GuardDescriptorOpWriter& writer) { \
        writer.writeRaw(reader.readerMethod());                \
        return true;                                           \
      }                                                        \
                                                               \
      static mozilla::Maybe<Source> Deserialize(               \
          GuardDescriptorOpReader& reader) {                   \
        return reader.readRaw<Source>();                       \
      }                                                        \
    };

SIMPLE_CODEC(bool, BoolImm, readBool)
SIMPLE_CODEC(uint8_t, ByteImm, readByte)
SIMPLE_CODEC(int32_t, Int32Imm, int32Immediate)
SIMPLE_CODEC(uint32_t, UInt32Imm, uint32Immediate)
SIMPLE_CODEC(JSOp, JSOpImm, jsop)
SIMPLE_CODEC(ValueType, ValueTypeImm, valueType)
SIMPLE_CODEC(Scalar::Type, ScalarTypeImm, scalarType)
SIMPLE_CODEC(UnaryMathFunction, UnaryMathFunctionImm, unaryMathFunction)
SIMPLE_CODEC(wasm::ValType::Kind, WasmValTypeImm, wasmValType)
SIMPLE_CODEC(gc::AllocKind, AllocKindImm, allocKind)
SIMPLE_CODEC(RealmFuses::FuseIndex, RealmFuseIndexImm, realmFuseIndex)
SIMPLE_CODEC(RuntimeFuses::FuseIndex, RuntimeFuseIndexImm, runtimeFuseIndex)
SIMPLE_CODEC(ArrayBufferViewKind, ArrayBufferViewKindImm, arrayBufferViewKind)
SIMPLE_CODEC(GuardClassKind, GuardClassKindImm, guardClassKind)

/*
BigIntId
BooleanId
CallFlagsImm
Int32Id
IntPtrId
NumberId
ObjId
RawId
StaticStringImm
StringId
SymbolId
TypeofEqOperandImm
ValId
ValueTagId
*/

GuardDescriptorCollector GuardDescriptorCollector::guardDescriptorCollector =
    GuardDescriptorCollector();

void GuardDescriptorCollector::collectStubStats(ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  CacheIRReader reader(info);
  do {
    CacheOp op = reader.readOp();
    ArgsList args = ArgsListOf(op);

    totalArgCounter += args.len;
    totalOpCounter++;
    ArgClassification opClassification = ArgClassification::Trivial;
    for (size_t i = 0; i < args.len; i++) {
      ArgClassification classification = ArgClassificationOf(args.kinds[i]);
      argCounters[args.kinds[i]]++;
      classificationCounters[classification]++;
      if (classification > opClassification) opClassification = classification;
    }
    if (args.len > 0) opMaxClassificationCounters[opClassification]++;
    uint32_t argLength = CacheIROpInfos[size_t(op)].argLength;
    reader.skip(argLength);
  } while (reader.more());
}

#  define ARGS_WITH_CODEC(_)                                        \
    _(ArgKind::BoolImm, BoolImmCodec)                               \
    _(ArgKind::ByteImm, ByteImmCodec)                               \
    _(ArgKind::Int32Imm, Int32ImmCodec)                             \
    _(ArgKind::UInt32Imm, UInt32ImmCodec)                           \
    _(ArgKind::JSOpImm, JSOpImmCodec)                               \
    _(ArgKind::ValueTypeImm, ValueTypeImmCodec)                     \
    _(ArgKind::ScalarTypeImm, ScalarTypeImmCodec)                   \
    _(ArgKind::UnaryMathFunctionImm, UnaryMathFunctionImmCodec)     \
    _(ArgKind::WasmValTypeImm, WasmValTypeImmCodec)                 \
    _(ArgKind::AllocKindImm, AllocKindImmCodec)                     \
    _(ArgKind::RealmFuseIndexImm, RealmFuseIndexImmCodec)           \
    _(ArgKind::RuntimeFuseIndexImm, RuntimeFuseIndexImmCodec)       \
    _(ArgKind::ArrayBufferViewKindImm, ArrayBufferViewKindImmCodec) \
    _(ArgKind::GuardClassKindImm, GuardClassKindImmCodec)

// TODO: There are a few remaining imms
// TODO:
// TODO: IDs need a remap table on deserialize

void GuardDescriptorCollector::collectStub(ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  CacheIRReader reader(info);
  do {
    CacheOp op = reader.readOp();
    ArgsList args = ArgsListOf(op);
    GuardDescriptorOpWriter writer(op);

    for (size_t i = 0; i < args.len; i++) {
      switch (args.kinds[i]) {
#  define CASE(kind, codec)           \
    case kind: {                      \
      writer.writeArg<codec>(reader); \
      break;                          \
    }
        ARGS_WITH_CODEC(CASE)
#  undef CASE
        default:
          return;
      }
    }

  } while (reader.more());
}

#  undef ARGS_WITH_CODEC

void GuardDescriptorCollector::dumpStats(GenericPrinter& printer) {
  JSONPrinter jsonPrinter(printer);

  std::map<ArgClassification, std::vector<ArgKind>> classes;

  jsonPrinter.beginObject();

  jsonPrinter.beginObjectProperty("argCount");
  for (auto& counterPair : argCounters) {
    jsonPrinter.property(ArgKindName(counterPair.first), counterPair.second);
    classes[ArgClassificationOf(counterPair.first)].push_back(
        counterPair.first);
  }
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("classCount");
  for (auto& counterPair : classificationCounters) {
    jsonPrinter.property(ArgClassificationName(counterPair.first),
                         counterPair.second);
  }
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("opMaxClassCount");
  for (auto& counterPair : opMaxClassificationCounters) {
    jsonPrinter.property(ArgClassificationName(counterPair.first),
                         counterPair.second);
  }
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("totalCount");
  jsonPrinter.property("op", totalOpCounter);
  jsonPrinter.property("arg", totalArgCounter);
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("argClass");
  for (auto& classPair : classes) {
    jsonPrinter.beginListProperty(ArgClassificationName(classPair.first));
    for (auto& arg : classPair.second) {
      jsonPrinter.value("%s", ArgKindName(arg));
    }
    jsonPrinter.endList();
  }
  jsonPrinter.endObject();

  jsonPrinter.endObject();
}

void GuardDescriptorCollector::dumpStats() {
  FILE* file = fopen("guardDescriptorStats.json", "w");
  Fprinter printer(file);
  dumpStats(printer);
}

#endif /* JS_GUARD_DESCRIPTORS */
