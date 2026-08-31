/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/TypeDecls.h"
#include "vm/GlobalObject.h"
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
#  include "vm/ArrayObject.h"
#  include "vm/JSContext.h"
#  include "vm/JSONPrinter.h"
#  include "vm/PlainObject.h"
#  include "vm/PropMap.h"
#  include "vm/RealmFuses.h"
#  include "vm/RuntimeFuses.h"
#  include "vm/Shape.h"
#  include "vm/TaggedProto.h"
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
  // TODO: Add more well-known prototypes
};

#  define SUPPORTED_CLASSES(_)          \
    _(PlainObject, PlainObject::class_) \
    _(ArrayObject, ArrayObject::class_)
// TODO: Add more well-known classes

// Represents all valid well-known classes, such as PlainObject.
// Used for shape serialization.
//
// TODO: Guard against version changes
enum class WellKnownClass : uint8_t {
#  define CLASS_VAR(var, _) var,
  SUPPORTED_CLASSES(CLASS_VAR)
#  undef CLASS_VAR
};

mozilla::Maybe<WellKnownClass> classToEnum(const JSClass* clazz) {
  using enum WellKnownClass;
  // TODO: Consider switching this to a hashmap indexed by the JSClass*
#  define CLASS_CONVERT(var, cls) \
    if (clazz == &cls) {          \
      return mozilla::Some(var);  \
    }
  SUPPORTED_CLASSES(CLASS_CONVERT)
#  undef CLASS_CONVERT
  return mozilla::Nothing();
}

const JSClass* enumToClass(WellKnownClass clazz) {
  using enum WellKnownClass;
  switch (clazz) {
#  define ENUM_CONVERT(var, cls) \
    case var:                    \
      return &cls;
    SUPPORTED_CLASSES(ENUM_CONVERT)
#  undef ENUM_CONVERT
  }
}

#  undef SUPPORTED_CLASSES

// A data structure to hold the relevant data for reading CacheIR and
// parsing its data structures. Used during the serialization process.
struct CacheIRReadData {
  JSContext* context;
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
//
// NOTE: A new system may needed to allow dynamically sized values
// as currently only statically sized serialized values are supported
template <typename C>
concept CacheIRCodec =
    requires(C::Source src, C::Dest dst, CacheIRReadData& readData,
             CacheIRWriteData& writeData) {
      // The source data type of this codec
      typename C::Source;
      // The serialized destination type of this codec. This must be trivially
      // copyable.
      typename C::Dest;
      requires std::is_trivially_copyable_v<typename C::Dest>;

      // Takes the read value and tries to convert it to a serializable form.
      // Returns Some(Dest) if it successfully serializes the value, which will
      // be written out. Returns Nothing() if it fails to serialize the value,
      // causing a serialization failure.
      { C::Serialize(src) } -> std::same_as<mozilla::Maybe<typename C::Dest>>;

      // Reads the serialized data from gReader and reconstructs it
      // using writeData, returning Some(data) if it was successfully
      // deserialized, and Nothing() otherwise.
      //
      // Takes an automatically read set of bytes in the form of Dest and must
      // convert it back to Source. Also recieves writeData to help re-create
      // the value. This operation should be infallible.
      { C::Deserialize(dst, writeData) } -> std::same_as<typename C::Source>;
    };

template <typename C>
concept CacheIRCodecExt = requires(CacheIRReadData& readData) {
  requires CacheIRCodec<C>;

  // Reads from readData to produce some source value, returning it.
  // Performs no serialization, only extracts the value.
  //
  // SAFETY: This function MUST consume the appropriate quantity
  // of data from the CacheIRReader, as this is a side-effect
  // that is propagated forwards.
  { C::Read(readData) } -> std::same_as<typename C::Source>;
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

  template <typename T>
  void writeRaw(T val) {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) != 0, "Unsupported width for writeRaw");
    uint8_t* source = reinterpret_cast<uint8_t*>(&val);
    for (size_t i = 0; i < sizeof(T); i++) {
      bytes.push_back(source[i]);
    }
  }

  // Tries to write an argument with the given codec to the internal buffer.
  // Returns true if it was serialized, false if it failed.
  // An op which fails to have an argument written must NEVER be written out.
  template <typename Codec>
  void writeArg(CacheIRReadData& readData)
    requires CacheIRCodecExt<Codec>
  {
    if (!failed_) {
      mozilla::Maybe<typename Codec::Dest> val =
          Codec::Serialize(Codec::Read(readData));
      failed_ = val.isNothing();
      val.apply([&](typename Codec::Dest dst) { writeRaw(dst); });
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

  template <typename T>
  mozilla::Maybe<T> readRaw() {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) != 0, "Unsupported width for readRaw");
    if (pos_ + sizeof(T) > bytes.Length()) {
      return mozilla::Nothing();
    }

    T val;
    std::memcpy(&val, bytes.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return mozilla::Some(val);
  }

  template <typename Codec>
  mozilla::Maybe<typename Codec::Source> readArg(CacheIRWriteData& writeData)
    requires CacheIRCodec<Codec>
  {
    mozilla::Maybe<typename Codec::Dest> val = readRaw<typename Codec::Dest>();
    return val.andThen([&](typename Codec::Dest dst) {
      return Codec::Deserialize(dst, writeData);
    });
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
#  define IMM_CODEC(ty, name, readerMethod)                              \
    struct name##Codec {                                                 \
      using Source = ty;                                                 \
      using Dest = ty;                                                   \
                                                                         \
      static Source Read(CacheIRReadData& readData) {                    \
        return readData.reader.readerMethod();                           \
      }                                                                  \
                                                                         \
      static mozilla::Maybe<Dest> Serialize(Source src) {                \
        return mozilla::Some(src);                                       \
      }                                                                  \
                                                                         \
      static Source Deserialize(Dest dst, CacheIRWriteData& writeData) { \
        return dst;                                                      \
      }                                                                  \
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
#  define ID_CODEC(ty, name, readerMethod)                               \
    struct name##Codec {                                                 \
      using Source = ty;                                                 \
      using Dest = uint16_t;                                             \
                                                                         \
      static Source Read(CacheIRReadData& readData) {                    \
        return readData.reader.readerMethod();                           \
      }                                                                  \
                                                                         \
      static mozilla::Maybe<Dest> Serialize(Source src) {                \
        return mozilla::Some(src.id());                                  \
      }                                                                  \
                                                                         \
      static Source Deserialize(Dest dst, CacheIRWriteData& writeData) { \
        return ty(dst);                                                  \
      }                                                                  \
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

// TODO: Finish serialization for Shape
struct ShapeCodec {
  struct SerializedShape {
    WellKnownClass cls;
    WellKnownPrototype proto;
    // TODO: Add all other values to be serialized
  };

  using Source = Shape*;
  using Dest = SerializedShape;

  // TODO: Needs all the properties of a serialized shape;

  static mozilla::Maybe<Dest> Serialize(Source val) {
    // TODO: Handle cases that are not SharedShape, this is a prototype
    if (!val->isShared()) {
      return mozilla::Nothing();
    }
    SharedShape& shared = val->asShared();
    SharedPropMap* propMap = shared.propMap();
    TaggedProto proto = shared.proto();

    // TODO: Check prototype to see if it is well-known
    // TODO: Check prototype with fuse to see if its been modified
    // since a modified well-known prototype must be considered
    // different, and thus, unserializable

    SerializedShape serialized = {
        WellKnownClass::PlainObject,  // This should be found, not constant
        WellKnownPrototype::Object    // This should be found, not constant
    };

    return mozilla::Some(serialized);
  }

  static Source Deserialize(Dest dst, CacheIRWriteData& writeData) {
    JS::Rooted<SharedPropMap*> map(writeData.context);
    JSClass a;
    ObjectFlags b;
    uint32_t slot = 0;
    SharedPropMap::addProperty(writeData.context, &a, &map, 0, 0,
                               PropertyFlags::defaultDataPropFlags, &b, &slot);

    // JSProtoKey::JSProto_DataView

    // return reader.readRaw<uint16_t>().map([](uint16_t id) { return
    // ty(id);
    // });

    // SharedShape::getPropMapShape(JSContext *cx, BaseShape *base,
    // size_t nfixed, Handle<SharedPropMap *> map, uint32_t mapLength,
    // ObjectFlags objectFlags)
  }
};

// Inherit from ShapeCodec as shared code, only unique part is the ShapeCodecExt
// static Read method
struct ShapeFieldCodec : ShapeCodec {
  static Source Read(CacheIRReadData& readData) {
    uint32_t stubOffset = readData.reader.stubOffset();
    auto shape = readData.info->getStubField<StubField::Type::Shape>(
        readData.stub, stubOffset);
    return shape;
  }
};

struct WeakShapeFieldCodec : ShapeCodec {
  static Source Read(CacheIRReadData& readData) {
    uint32_t stubOffset = readData.reader.stubOffset();
    auto weakShape = readData.info->getStubField<StubField::Type::WeakShape>(
        readData.stub, stubOffset);
    // TODO: Determine if weak shape can disappear before we use it
    Shape* shape = weakShape.get();
    return shape;
  }
};

/*
CallFlagsImm       - Requires adding a friend clause to read helper
values RawId              - Requires a return value of OperandId, as
per CacheIRWriter StaticStringImm    - Must cast pointer to char* and
read out string TypeofEqOperandImm
*/

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

GuardDescriptorCollector GuardDescriptorCollector::guardDescriptorCollector =
    GuardDescriptorCollector();

void GuardDescriptorCollector::collectStubStats(ICCacheIRStub* stub) {
  totalStubCounter++;
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

      switch (args.kinds[i]) {
#  define DO_NOTHING(kind, codec) \
    case kind:                    \
      break;
        ARGS_WITH_CODEC(DO_NOTHING);
#  undef DO_NOTHING
        default: {
          missingCodecCounter++;
          missingCodecOps[op].insert(args.kinds[i]);
        }
      }
    }
    if (args.len > 0) opMaxClassificationCounters[opClassification]++;
    uint32_t argLength = CacheIROpInfos[size_t(op)].argLength;
    reader.skip(argLength);
  } while (reader.more());
}

void GuardDescriptorCollector::collectStub(JSContext* cx, ICCacheIRStub* stub) {
  collectStubStats(stub);

  const CacheIRStubInfo* info = stub->stubInfo();
  CacheIRReader reader(info);
  CacheIRReadData readData = {cx, stub, info, reader};
  GuardDescriptorStubWriter stubWriter;

  do {
    CacheOp op = reader.readOp();
    ArgsList args = ArgsListOf(op);
    GuardDescriptorOpWriter writer(op);

    for (size_t i = 0; i < args.len; i++) {
      switch (args.kinds[i]) {
#  define CASE(kind, codec)             \
    case kind: {                        \
      writer.writeArg<codec>(readData); \
      break;                            \
    }
        ARGS_WITH_CODEC(CASE)
#  undef CASE
        default:
          return;
      }
      if (writer.failed()) {
        failedCodecCounter++;
        return;
      }
      successfulCodecCounter++;
    }

    stubWriter.writeOp(writer);
    completeOpCounter++;

  } while (reader.more());

  completeStubCounter++;
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

  jsonPrinter.beginObjectProperty("argClass");
  for (auto& classPair : classes) {
    jsonPrinter.beginListProperty(ArgClassificationName(classPair.first));
    for (auto& arg : classPair.second) {
      jsonPrinter.value("%s", ArgKindName(arg));
    }
    jsonPrinter.endList();
  }
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("missingCodecs");
  for (auto& opPair : missingCodecOps) {
    jsonPrinter.beginListProperty(CacheIRCodeName(opPair.first));
    for (auto& arg : opPair.second) {
      jsonPrinter.value("%s", ArgKindName(arg));
    }
    jsonPrinter.endList();
  }
  jsonPrinter.endObject();

  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("counts");
  jsonPrinter.property("arg", totalArgCounter);
  jsonPrinter.property("op", totalOpCounter);
  jsonPrinter.property("stub", totalStubCounter);
  jsonPrinter.property("missingCodec", missingCodecCounter);
  jsonPrinter.endObject();

  jsonPrinter.beginObjectProperty("serializationStats");

  jsonPrinter.property("successfulCodec", successfulCodecCounter);
  jsonPrinter.property("failedCodec", failedCodecCounter);
  jsonPrinter.property("completeOp", completeOpCounter);
  jsonPrinter.property("completeStub", completeStubCounter);
  jsonPrinter.beginObjectProperty("failures");
  for (auto& opPair : failedCodecOpCounters) {
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
