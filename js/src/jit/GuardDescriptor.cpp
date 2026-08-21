/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/RootingAPI.h"
#include "jsapi.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"
#include "vm/PropMap.h"
#include "vm/PropertyInfo.h"
#include "vm/Shape.h"
#ifdef JS_GUARD_DESCRIPTORS
#  include "jit/GuardDescriptor.h"

#  include "mozilla/Maybe.h"
#  include "mozilla/Span.h"

#  include <concepts>
#  include <cstdint>
#  include <cstring>
#  include <map>
#  include <type_traits>
#  include <utility>
#  include <vector>

#  include "builtin/Math.h"
#  include "gc/AllocKind.h"
#  include "jit/BaselineIC.h"
#  include "jit/CacheIR.h"
#  include "jit/CacheIRCompiler.h"
#  include "jit/CacheIRReader.h"
#  include "jit/CacheIRWriter.h"
#  include "jit/GuardDescriptorArgKindsGenerated.h"
#  include "js/Printer.h"
#  include "js/ScalarType.h"
#  include "vm/JSONPrinter.h"
#  include "vm/RealmFuses.h"
#  include "vm/RuntimeFuses.h"
#  include "wasm/WasmTypeDecls.h"

using namespace js;
using namespace js::jit;

// Creates an enum of all CacheIR op argument kinds. Uses an
// auto-generated macro from GenerateGuardDescriptorArgKinds.py.
enum class GuardDescriptorCollector::ArgKind : uint8_t {
#  define DEFINE_ARG_KIND(kind) kind,
  GUARD_DESCRIPTOR_ARG_KINDS(DEFINE_ARG_KIND)
#  undef DEFINE_ARG_KIND
};

// Creates an enum of all argument classifications. Uses an
// auto-generated macro from GenerateGuardDescriptorArgKinds.py.
enum class GuardDescriptorCollector::ArgClassification : uint8_t {
#  define DEFINE_CLASSIFICATION_KIND(kind) kind,
  GUARD_DESCRIPTOR_CLASSIFICATION_KINDS(DEFINE_CLASSIFICATION_KIND)
#  undef DEFINE_CLASSIFICATION_KIND
};

// Creates const arrays of the argument kinds for each op in order.
// Can be accessed by name, or through ArgsListOf(op). Uses an
// auto-generated macro from GenerateGuardDescriptorArgKinds.py.
using enum GuardDescriptorCollector::ArgKind;
#  define DEFINE_OP_ARG_KINDS(op, len, ...) \
    constexpr GuardDescriptorCollector::ArgKind op##ArgKinds[] = {__VA_ARGS__};
GUARD_DESCRIPTOR_OP_ARG_KINDS(DEFINE_OP_ARG_KINDS)
#  undef DEFINE_OP_ARG_KINDS

// Represents the list of arguments for a given CacheIR op
struct ArgsList {
  const GuardDescriptorCollector::ArgKind* kinds;
  size_t len;
};

// Maps a CacheIR op to the appropriate above defined list of arg
// kinds. Uses an auto-generated macro from
// GenerateGuardDescriptorArgKinds.py.
ArgsList ArgsListOf(CacheOp op) {
  switch (op) {
    using enum CacheOp;
#  define OPARGS(op, len, ...) \
    case op:                   \
      return {op##ArgKinds, len};
    GUARD_DESCRIPTOR_OP_ARG_KINDS(OPARGS)
#  undef OPARGS
    default:
      return {{}, 0};  // TODO: Add some failure condition, this ideally should
                       // not just be an empty return
  }
}

// Maps a CacheIR op arg to the arg's classification. Uses an
// auto-generated macro from GenerateGuardDescriptorArgKinds.py.
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

// Converts an arg kind to its string name for printing and debug.
// Uses an auto-generated macro from GenerateGuardDescriptorArgKinds.py.
const char* ArgKindName(GuardDescriptorCollector::ArgKind arg) {
  switch (arg) {
#  define ARG_KIND_NAME(kind) \
    case kind:                \
      return #kind;
    GUARD_DESCRIPTOR_ARG_KINDS(ARG_KIND_NAME)
#  undef ARG_KIND_NAME
  }
}

// Converts an arg classification to its string name for printing and debug.
// Uses an auto-generated macro from GenerateGuardDescriptorArgKinds.py.
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

// Represents all valid well-known prototypes, such as Object.prototype
// or Array.prototype. Used for shape and prototype serialization.
//
// TODO: Guard against version changes
enum class WellKnownPrototype : uint8_t {
  Object,
  Function,
  Array,
};

// A data structure to hold the relevant data for reading CacheIR and
// parsing its data structures. Used during the serialization process.
struct CacheIRReadData {
  ICCacheIRStub* stub;
  const CacheIRStubInfo* info;
  CacheIRReader& reader;
};

// A data structure to hold the relevant data for writing CacheIR and
// reconstructing its data structures. Used during the deserialization
// process.
struct CacheIRWriteData {
  JSContext* context;
};

class GuardDescriptorOpWriter;
class GuardDescriptorOpReader;

// A concept for templates to require a valid codec be passed.
// A CacheIRCodec represents a pairing of a serialization and
// deserialization function for a single arg kind, but contains
// no actual data or instance methods.
template <typename C>
concept CacheIRCodec = requires(
    CacheIRReadData& readData, CacheIRWriteData& writeData,
    GuardDescriptorOpWriter& gWriter, GuardDescriptorOpReader& gReader) {
  // The source data type of this codec
  typename C::Source;

  // Reads from readData to write a value to gWriter, returning
  // true if the value was successfully serialized, and false
  // if it failed.
  //
  // SAFETY: This function MUST consume the appropriate quantity
  // of data from the CacheIRReader, as this is a side-effect
  // that is propagated forwards.
  { C::Serialize(readData, gWriter) } -> std::same_as<bool>;

  // Reads the serialized data from gReader and reconstructs it
  // using writeData, returning Some(data) if it was successfully
  // deserialized, and Nothing() otherwise.
  //
  // SAFETY: This function MUST consume the appropriate quantity
  // of data from the GuardDescriptorOpReader, as this is a
  // side-effect that is propagated forwards.
  {
    C::Deserialize(writeData, gReader)
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
  void writeArg(CacheIRReadData& readData)
    requires CacheIRCodec<Codec>
  {
    if (!failed_) {
      failed_ = !Codec::Serialize(readData, *this);
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

// Stores the bytes of a guard descriptor stub. Can be adapted to encode
// additional bytes as a header/footer.
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

// Most immediates can just be written out as raw binary data, and then be
// read back, casted as the correct type.
#  define IMM_CODEC(ty, name, readerMethod)                               \
    struct name##Codec {                                                  \
      using Source = ty;                                                  \
      static bool Serialize(CacheIRReadData& readData,                    \
                            GuardDescriptorOpWriter& writer) {            \
        writer.writeRaw(readData.reader.readerMethod());                  \
        return true;                                                      \
      }                                                                   \
                                                                          \
      static mozilla::Maybe<Source> Deserialize(                          \
          CacheIRWriteData& writeData, GuardDescriptorOpReader& reader) { \
        return reader.readRaw<Source>();                                  \
      }                                                                   \
    };

IMM_CODEC(bool, BoolImm, readBool)
IMM_CODEC(uint8_t, ByteImm, readByte)
IMM_CODEC(int32_t, Int32Imm, int32Immediate)
IMM_CODEC(uint32_t, UInt32Imm, uint32Immediate)
IMM_CODEC(JSOp, JSOpImm, jsop)
IMM_CODEC(ValueType, ValueTypeImm, valueType)
IMM_CODEC(Scalar::Type, ScalarTypeImm, scalarType)
IMM_CODEC(UnaryMathFunction, UnaryMathFunctionImm, unaryMathFunction)
IMM_CODEC(wasm::ValType::Kind, WasmValTypeImm, wasmValType)
IMM_CODEC(gc::AllocKind, AllocKindImm, allocKind)
IMM_CODEC(RealmFuses::FuseIndex, RealmFuseIndexImm, realmFuseIndex)
IMM_CODEC(RuntimeFuses::FuseIndex, RuntimeFuseIndexImm, runtimeFuseIndex)
IMM_CODEC(ArrayBufferViewKind, ArrayBufferViewKindImm, arrayBufferViewKind)
IMM_CODEC(GuardClassKind, GuardClassKindImm, guardClassKind)

// Most IDs can be serialized directly by writing out their value, and then
// just reading them back in. The CacheIRWriter will need to be informed of
// the IDs being used, but for the most part, IDs are a trivial operation.
#  define ID_CODEC(ty, name, readerMethod)                                \
    struct name##Codec {                                                  \
      using Source = ty;                                                  \
      static bool Serialize(CacheIRReadData& readData,                    \
                            GuardDescriptorOpWriter& writer) {            \
        writer.writeRaw(readData.reader.readerMethod().id());             \
        return true;                                                      \
      }                                                                   \
                                                                          \
      static mozilla::Maybe<Source> Deserialize(                          \
          CacheIRWriteData& writeData, GuardDescriptorOpReader& reader) { \
        return reader.readRaw<uint16_t>().map(                            \
            [](uint16_t id) { return ty(id); });                          \
      }                                                                   \
    };

// TODO: Implement id setting on deserialization
ID_CODEC(BigIntOperandId, BigIntId, bigIntOperandId)
ID_CODEC(BooleanOperandId, BooleanId, booleanOperandId)
ID_CODEC(Int32OperandId, Int32Id, int32OperandId)
ID_CODEC(IntPtrOperandId, IntPtrId, intPtrOperandId)
ID_CODEC(NumberOperandId, NumberId, numberOperandId)
ID_CODEC(ObjOperandId, ObjId, objOperandId)
ID_CODEC(ValOperandId, ValId, valOperandId)
ID_CODEC(StringOperandId, StringId, stringOperandId)
ID_CODEC(SymbolOperandId, SymbolId, symbolOperandId)
ID_CODEC(ValueTagOperandId, ValueTagId, valueTagOperandId)

struct WeakShapeFieldCodec {
  using Source = Shape*;
  static bool Serialize(CacheIRReadData& readData,
                        GuardDescriptorOpWriter& writer) {
    uint32_t stubOffset = readData.reader.stubOffset();
    auto weakShape = readData.info->getStubField<StubField::Type::WeakShape>(
        readData.stub, stubOffset);
    // TODO: Determine if weak shape can disappear before we use it
    Shape* shape = weakShape.get();
    // shape->kind();
    // writer.writeRaw(read.readerMethod().id());
    return true;
  }

  static mozilla::Maybe<Source> Deserialize(CacheIRWriteData& writeData,
                                            GuardDescriptorOpReader& reader) {
    JS::Rooted<SharedPropMap*> map(writeData.context);
    JSClass a;
    ObjectFlags b;
    uint32_t slot = 0;
    SharedPropMap::addProperty(writeData.context, &a, &map, 0, 0,
                               PropertyFlags::defaultDataPropFlags, &b, &slot);

    // return reader.readRaw<uint16_t>().map([](uint16_t id) { return
    // ty(id);
    // });

    // SharedShape::getPropMapShape(JSContext *cx, BaseShape *base,
    // size_t nfixed, Handle<SharedPropMap *> map, uint32_t mapLength,
    // ObjectFlags objectFlags)
    return mozilla::Nothing();
  }
};

/*
CallFlagsImm       - Requires adding a friend clause to read helper
values RawId              - Requires a return value of OperandId, as
per CacheIRWriter StaticStringImm    - Must cast pointer to char* and
read out string TypeofEqOperandImm
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
    _(ArgKind::GuardClassKindImm, GuardClassKindImmCodec)           \
                                                                    \
    _(ArgKind::BigIntId, BigIntIdCodec)                             \
    _(ArgKind::BooleanId, BooleanIdCodec)                           \
    _(ArgKind::Int32Id, Int32IdCodec)                               \
    _(ArgKind::IntPtrId, IntPtrIdCodec)                             \
    _(ArgKind::NumberId, NumberIdCodec)                             \
    _(ArgKind::ObjId, ObjIdCodec)                                   \
    _(ArgKind::StringId, StringIdCodec)                             \
    _(ArgKind::SymbolId, SymbolIdCodec)                             \
    _(ArgKind::ValId, ValIdCodec)                                   \
    _(ArgKind::ValueTagId, ValueTagIdCodec)

void GuardDescriptorCollector::collectStub(ICCacheIRStub* stub) {
  const CacheIRStubInfo* info = stub->stubInfo();
  totalSeenStubCounter++;
  CacheIRReader reader(info);
  CacheIRReadData readData = {stub, info, reader};
  GuardDescriptorStubWriter stubWriter;

  do {
    CacheOp op = reader.readOp();
    totalSeenOpCounter++;
    ArgsList args = ArgsListOf(op);
    GuardDescriptorOpWriter writer(op);

    for (size_t i = 0; i < args.len; i++) {
      totalSeenArgCounter++;
      switch (args.kinds[i]) {
#  define CASE(kind, codec)             \
    case kind: {                        \
      writer.writeArg<codec>(readData); \
      break;                            \
    }
        ARGS_WITH_CODEC(CASE)
#  undef CASE
        default: {
          failedOpCounters[op][args.kinds[i]]++;
          return;
        }
      }
      if (writer.failed()) {
        failedOpCounters[op][args.kinds[i]]++;
        return;
      }

      serializedArgCounter++;
    }

    serializedOpCounter++;
    stubWriter.writeOp(writer);

  } while (reader.more());

  serializedStubCounter++;
}

#  undef ARGS_WITH_CODEC

void GuardDescriptorCollector::dumpStats(GenericPrinter& printer) {
  JSONPrinter jsonPrinter(printer);

  std::map<ArgClassification, std::vector<ArgKind>> classes;

  jsonPrinter.beginObject();

  jsonPrinter.beginObjectProperty("collectedStats");
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

  jsonPrinter.beginObjectProperty("serializationStats");

  jsonPrinter.beginObjectProperty("sums");
  jsonPrinter.property("totalArg", totalSeenArgCounter);
  jsonPrinter.property("serializedArg", serializedArgCounter);
  jsonPrinter.property("totalOp", totalSeenOpCounter);
  jsonPrinter.property("serializedOp", serializedOpCounter);
  jsonPrinter.property("totalStub", totalSeenStubCounter);
  jsonPrinter.property("serializedStub", serializedStubCounter);
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("failures");
  for (auto& opPair : failedOpCounters) {
    jsonPrinter.beginObjectProperty(CacheIRCodeName(opPair.first));
    for (auto& argPair : opPair.second) {
      jsonPrinter.property(ArgKindName(argPair.first), argPair.second);
    }
    jsonPrinter.endObject();
  }
  jsonPrinter.endObject();

  jsonPrinter.endObject();
  jsonPrinter.endObject();
}

void GuardDescriptorCollector::dumpStats() {
  FILE* file = fopen("guardDescriptorStats.json", "w");
  Fprinter printer(file);
  dumpStats(printer);
}

#endif /* JS_GUARD_DESCRIPTORS */
