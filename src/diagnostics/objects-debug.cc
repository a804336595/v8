// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/logging.h"
#include "src/codegen/assembler-inl.h"
#include "src/common/globals.h"
#include "src/date/date.h"
#include "src/diagnostics/disasm.h"
#include "src/diagnostics/disassembler.h"
#include "src/heap/combined-heap.h"
#include "src/heap/heap-write-barrier-inl.h"
#include "src/heap/read-only-heap.h"
#include "src/ic/handler-configuration-inl.h"
#include "src/init/bootstrapper.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/allocation-site-inl.h"
#include "src/objects/arguments-inl.h"
#include "src/objects/bigint.h"
#include "src/objects/call-site-info-inl.h"
#include "src/objects/cell-inl.h"
#include "src/objects/code-inl.h"
#include "src/objects/data-handler-inl.h"
#include "src/objects/debug-objects-inl.h"
#include "src/objects/elements.h"
#include "src/objects/embedder-data-array-inl.h"
#include "src/objects/embedder-data-slot-inl.h"
#include "src/objects/feedback-cell-inl.h"
#include "src/objects/field-type.h"
#include "src/objects/foreign-inl.h"
#include "src/objects/free-space-inl.h"
#include "src/objects/function-kind.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/instance-type.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-atomics-synchronization-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/objects.h"
#include "src/objects/turbofan-types-inl.h"
#include "src/objects/turboshaft-types-inl.h"
#include "src/roots/roots.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/js-break-iterator-inl.h"
#include "src/objects/js-collator-inl.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/js-collection-inl.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/js-date-time-format-inl.h"
#include "src/objects/js-display-names-inl.h"
#include "src/objects/js-duration-format-inl.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/js-generator-inl.h"
#include "src/objects/js-iterator-helpers-inl.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/js-list-format-inl.h"
#include "src/objects/js-locale-inl.h"
#include "src/objects/js-number-format-inl.h"
#include "src/objects/js-plural-rules-inl.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/js-regexp-inl.h"
#include "src/objects/js-regexp-string-iterator-inl.h"
#include "src/objects/js-shadow-realm-inl.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/js-relative-time-format-inl.h"
#include "src/objects/js-segment-iterator-inl.h"
#include "src/objects/js-segmenter-inl.h"
#include "src/objects/js-segments-inl.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/hole-inl.h"
#include "src/objects/js-raw-json-inl.h"
#include "src/objects/js-shared-array-inl.h"
#include "src/objects/js-struct-inl.h"
#include "src/objects/js-temporal-objects-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/objects/literal-objects-inl.h"
#include "src/objects/maybe-object.h"
#include "src/objects/megadom-handler-inl.h"
#include "src/objects/microtask-inl.h"
#include "src/objects/module-inl.h"
#include "src/objects/oddball-inl.h"
#include "src/objects/promise-inl.h"
#include "src/objects/property-descriptor-object-inl.h"
#include "src/objects/struct-inl.h"
#include "src/objects/swiss-name-dictionary-inl.h"
#include "src/objects/synthetic-module-inl.h"
#include "src/objects/template-objects-inl.h"
#include "src/objects/torque-defined-classes-inl.h"
#include "src/objects/transitions-inl.h"
#include "src/regexp/regexp.h"
#include "src/utils/ostreams.h"
#include "torque-generated/class-verifiers.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/base/strings.h"
#include "src/debug/debug-wasm-objects-inl.h"
#include "src/wasm/wasm-objects-inl.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

// Heap Verification Overview
// --------------------------
// - Each InstanceType has a separate XXXVerify method which checks an object's
//   integrity in isolation.
// - --verify-heap will iterate over all gc spaces and call ObjectVerify() on
//   every encountered tagged pointer.
// - Verification should be pushed down to the specific instance type if its
//   integrity is independent of an outer object.
// - In cases where the InstanceType is too generic (e.g. FixedArray) the
//   XXXVerify of the outer method has to do recursive verification.
// - If the corresponding objects have inheritence the parent's Verify method
//   is called as well.
// - For any field containing pointes VerifyPointer(...) should be called.
//
// Caveats
// -------
// - Assume that any of the verify methods is incomplete!
// - Some integrity checks are only partially done due to objects being in
//   partially initialized states when a gc happens, for instance when outer
//   objects are allocted before inner ones.
//

#ifdef VERIFY_HEAP

#define USE_TORQUE_VERIFIER(Class)                                \
  void Class::Class##Verify(Isolate* isolate) {                   \
    TorqueGeneratedClassVerifiers::Class##Verify(*this, isolate); \
  }

// static
void Object::ObjectVerify(Tagged<Object> obj, Isolate* isolate) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kObjectVerify);
  if (IsSmi(obj)) {
    Smi::SmiVerify(Smi::cast(obj), isolate);
  } else {
    HeapObject::cast(obj)->HeapObjectVerify(isolate);
  }
  PtrComprCageBase cage_base(isolate);
  CHECK(!IsConstructor(obj, cage_base) || IsCallable(obj, cage_base));
}

void Object::VerifyPointer(Isolate* isolate, Object p) {
  if (IsHeapObject(p)) {
    HeapObject::VerifyHeapPointer(isolate, p);
  } else {
    CHECK(IsSmi(p));
  }
}

void Object::VerifyAnyTagged(Isolate* isolate, Object p) {
  if (IsHeapObject(p)) {
    if (V8_EXTERNAL_CODE_SPACE_BOOL) {
      CHECK(IsValidHeapObject(isolate->heap(), HeapObject::cast(p)));
    } else {
      HeapObject::VerifyHeapPointer(isolate, p);
    }
  } else {
    CHECK(IsSmi(p));
  }
}

void MaybeObject::VerifyMaybeObjectPointer(Isolate* isolate, MaybeObject p) {
  HeapObject heap_object;
  if (p->GetHeapObject(&heap_object)) {
    HeapObject::VerifyHeapPointer(isolate, heap_object);
  } else {
    CHECK(p->IsSmi() || p->IsCleared() || MapWord::IsPacked(p->ptr()));
  }
}

// static
void Smi::SmiVerify(Tagged<Smi> obj, Isolate* isolate) {
  CHECK(IsSmi(obj));
  CHECK(!IsCallable(obj));
  CHECK(!IsConstructor(obj));
}

// static
void TaggedIndex::TaggedIndexVerify(Tagged<TaggedIndex> obj, Isolate* isolate) {
  CHECK(IsTaggedIndex(obj));
}

void HeapObject::HeapObjectVerify(Isolate* isolate) {
  CHECK(IsHeapObject(*this));
  PtrComprCageBase cage_base(isolate);
  VerifyPointer(isolate, map(cage_base));
  CHECK(IsMap(map(cage_base), cage_base));

  CHECK(CheckRequiredAlignment(isolate));

  switch (map(cage_base)->instance_type()) {
#define STRING_TYPE_CASE(TYPE, size, name, CamelName) case TYPE:
    STRING_TYPE_LIST(STRING_TYPE_CASE)
#undef STRING_TYPE_CASE
    if (IsConsString(*this, cage_base)) {
      ConsString::cast(*this)->ConsStringVerify(isolate);
    } else if (IsSlicedString(*this, cage_base)) {
      SlicedString::cast(*this)->SlicedStringVerify(isolate);
    } else if (IsThinString(*this, cage_base)) {
      ThinString::cast(*this)->ThinStringVerify(isolate);
    } else if (IsSeqString(*this, cage_base)) {
      SeqString::cast(*this)->SeqStringVerify(isolate);
    } else if (IsExternalString(*this, cage_base)) {
      ExternalString::cast(*this)->ExternalStringVerify(isolate);
    } else {
      String::cast(*this)->StringVerify(isolate);
    }
    break;
    case OBJECT_BOILERPLATE_DESCRIPTION_TYPE:
      ObjectBoilerplateDescription::cast(*this)
          ->ObjectBoilerplateDescriptionVerify(isolate);
      break;
    // FixedArray types
    case CLOSURE_FEEDBACK_CELL_ARRAY_TYPE:
    case HASH_TABLE_TYPE:
    case ORDERED_HASH_MAP_TYPE:
    case ORDERED_HASH_SET_TYPE:
    case ORDERED_NAME_DICTIONARY_TYPE:
    case NAME_TO_INDEX_HASH_TABLE_TYPE:
    case REGISTERED_SYMBOL_TABLE_TYPE:
    case NAME_DICTIONARY_TYPE:
    case GLOBAL_DICTIONARY_TYPE:
    case NUMBER_DICTIONARY_TYPE:
    case SIMPLE_NUMBER_DICTIONARY_TYPE:
    case EPHEMERON_HASH_TABLE_TYPE:
    case SCRIPT_CONTEXT_TABLE_TYPE:
      FixedArray::cast(*this)->FixedArrayVerify(isolate);
      break;
    case AWAIT_CONTEXT_TYPE:
    case BLOCK_CONTEXT_TYPE:
    case CATCH_CONTEXT_TYPE:
    case DEBUG_EVALUATE_CONTEXT_TYPE:
    case EVAL_CONTEXT_TYPE:
    case FUNCTION_CONTEXT_TYPE:
    case MODULE_CONTEXT_TYPE:
    case SCRIPT_CONTEXT_TYPE:
    case WITH_CONTEXT_TYPE:
      Context::cast(*this)->ContextVerify(isolate);
      break;
    case NATIVE_CONTEXT_TYPE:
      NativeContext::cast(*this)->NativeContextVerify(isolate);
      break;
    case FEEDBACK_METADATA_TYPE:
      FeedbackMetadata::cast(*this)->FeedbackMetadataVerify(isolate);
      break;
    case TRANSITION_ARRAY_TYPE:
      TransitionArray::cast(*this)->TransitionArrayVerify(isolate);
      break;

    case INSTRUCTION_STREAM_TYPE:
      InstructionStream::cast(*this)->InstructionStreamVerify(isolate);
      break;
    case JS_API_OBJECT_TYPE:
    case JS_ARRAY_ITERATOR_PROTOTYPE_TYPE:
    case JS_CONTEXT_EXTENSION_OBJECT_TYPE:
    case JS_ERROR_TYPE:
    case JS_ITERATOR_PROTOTYPE_TYPE:
    case JS_MAP_ITERATOR_PROTOTYPE_TYPE:
    case JS_OBJECT_PROTOTYPE_TYPE:
    case JS_PROMISE_PROTOTYPE_TYPE:
    case JS_REG_EXP_PROTOTYPE_TYPE:
    case JS_SET_ITERATOR_PROTOTYPE_TYPE:
    case JS_SET_PROTOTYPE_TYPE:
    case JS_SPECIAL_API_OBJECT_TYPE:
    case JS_STRING_ITERATOR_PROTOTYPE_TYPE:
    case JS_TYPED_ARRAY_PROTOTYPE_TYPE:
      JSObject::cast(*this)->JSObjectVerify(isolate);
      break;
#if V8_ENABLE_WEBASSEMBLY
    case WASM_INSTANCE_OBJECT_TYPE:
      WasmInstanceObject::cast(*this)->WasmInstanceObjectVerify(isolate);
      break;
    case WASM_VALUE_OBJECT_TYPE:
      WasmValueObject::cast(*this)->WasmValueObjectVerify(isolate);
      break;
    case WASM_EXCEPTION_PACKAGE_TYPE:
      WasmExceptionPackage::cast(*this)->WasmExceptionPackageVerify(isolate);
      break;
#endif  // V8_ENABLE_WEBASSEMBLY
    case JS_SET_KEY_VALUE_ITERATOR_TYPE:
    case JS_SET_VALUE_ITERATOR_TYPE:
      JSSetIterator::cast(*this)->JSSetIteratorVerify(isolate);
      break;
    case JS_MAP_KEY_ITERATOR_TYPE:
    case JS_MAP_KEY_VALUE_ITERATOR_TYPE:
    case JS_MAP_VALUE_ITERATOR_TYPE:
      JSMapIterator::cast(*this)->JSMapIteratorVerify(isolate);
      break;
    case FILLER_TYPE:
      break;
    case CODE_TYPE:
      Code::cast(*this)->CodeVerify(isolate);
      break;

#define MAKE_TORQUE_CASE(Name, TYPE)          \
  case TYPE:                                  \
    Name::cast(*this)->Name##Verify(isolate); \
    break;
      // Every class that has its fields defined in a .tq file and corresponds
      // to exactly one InstanceType value is included in the following list.
      TORQUE_INSTANCE_CHECKERS_SINGLE_FULLY_DEFINED(MAKE_TORQUE_CASE)
      TORQUE_INSTANCE_CHECKERS_MULTIPLE_FULLY_DEFINED(MAKE_TORQUE_CASE)
#undef MAKE_TORQUE_CASE

    case ALLOCATION_SITE_TYPE:
      AllocationSite::cast(*this)->AllocationSiteVerify(isolate);
      break;

    case LOAD_HANDLER_TYPE:
      LoadHandler::cast(*this)->LoadHandlerVerify(isolate);
      break;

    case STORE_HANDLER_TYPE:
      StoreHandler::cast(*this)->StoreHandlerVerify(isolate);
      break;

    case BIG_INT_BASE_TYPE:
      BigIntBase::cast(*this)->BigIntBaseVerify(isolate);
      break;

    case JS_CLASS_CONSTRUCTOR_TYPE:
    case JS_PROMISE_CONSTRUCTOR_TYPE:
    case JS_REG_EXP_CONSTRUCTOR_TYPE:
    case JS_ARRAY_CONSTRUCTOR_TYPE:
#define TYPED_ARRAY_CONSTRUCTORS_SWITCH(Type, type, TYPE, Ctype) \
  case TYPE##_TYPED_ARRAY_CONSTRUCTOR_TYPE:
      TYPED_ARRAYS(TYPED_ARRAY_CONSTRUCTORS_SWITCH)
#undef TYPED_ARRAY_CONSTRUCTORS_SWITCH
      JSFunction::cast(*this)->JSFunctionVerify(isolate);
      break;
    case JS_LAST_DUMMY_API_OBJECT_TYPE:
      UNREACHABLE();
  }
}

// static
void HeapObject::VerifyHeapPointer(Isolate* isolate, Object p) {
  CHECK(IsHeapObject(p));
  // If you crashed here and {isolate->is_shared()}, there is a bug causing the
  // host of {p} to point to a non-shared object.
  CHECK(IsValidHeapObject(isolate->heap(), HeapObject::cast(p)));
  CHECK_IMPLIES(V8_EXTERNAL_CODE_SPACE_BOOL, !IsInstructionStream(p));
}

// static
void HeapObject::VerifyCodePointer(Isolate* isolate, Object p) {
  CHECK(IsHeapObject(p));
  CHECK(IsValidCodeObject(isolate->heap(), HeapObject::cast(p)));
  PtrComprCageBase cage_base(isolate);
  CHECK(IsInstructionStream(HeapObject::cast(p), cage_base));
}

void Symbol::SymbolVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::SymbolVerify(*this, isolate);
  uint32_t hash;
  const bool has_hash = TryGetHash(&hash);
  CHECK(has_hash);
  CHECK_GT(hash, 0);
  CHECK(IsUndefined(description(), isolate) || IsString(description()));
  CHECK_IMPLIES(IsPrivateName(), IsPrivate());
  CHECK_IMPLIES(IsPrivateBrand(), IsPrivateName());
}

void BytecodeArray::BytecodeArrayVerify(Isolate* isolate) {
  // TODO(oth): Walk bytecodes and immediate values to validate sanity.
  // - All bytecodes are known and well formed.
  // - Jumps must go to new instructions starts.
  // - No Illegal bytecodes.
  // - No consecutive sequences of prefix Wide / ExtraWide.
  TorqueGeneratedClassVerifiers::BytecodeArrayVerify(*this, isolate);
  for (int i = 0; i < constant_pool(isolate)->length(); ++i) {
    // No ThinStrings in the constant pool.
    CHECK(!IsThinString(constant_pool(isolate)->get(isolate, i), isolate));
  }
}

bool JSObject::ElementsAreSafeToExamine(PtrComprCageBase cage_base) const {
  // If a GC was caused while constructing this object, the elements
  // pointer may point to a one pointer filler map.
  return elements(cage_base) !=
         GetReadOnlyRoots(cage_base).one_pointer_filler_map();
}

namespace {

void VerifyJSObjectElements(Isolate* isolate, JSObject object) {
  // Only TypedArrays can have these specialized elements.
  if (IsJSTypedArray(object)) {
    // TODO(bmeurer,v8:4153): Fix CreateTypedArray to either not instantiate
    // the object or propertly initialize it on errors during construction.
    /* CHECK(object->HasTypedArrayOrRabGsabTypedArrayElements()); */
    return;
  }
  CHECK(!IsByteArray(object->elements()));

  if (object->HasDoubleElements()) {
    if (object->elements()->length() > 0) {
      CHECK(IsFixedDoubleArray(object->elements()));
    }
    return;
  }

  if (object->HasSloppyArgumentsElements()) {
    CHECK(IsSloppyArgumentsElements(object->elements()));
    return;
  }

  FixedArray elements = FixedArray::cast(object->elements());
  if (object->HasSmiElements()) {
    // We might have a partially initialized backing store, in which case we
    // allow the hole + smi values.
    for (int i = 0; i < elements->length(); i++) {
      Object value = elements->get(i);
      CHECK(IsSmi(value) || IsTheHole(value, isolate));
    }
  } else if (object->HasObjectElements()) {
    for (int i = 0; i < elements->length(); i++) {
      Object element = elements->get(i);
      CHECK(!HasWeakHeapObjectTag(element));
    }
  }
}
}  // namespace

void JSObject::JSObjectVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSObjectVerify(*this, isolate);
  VerifyHeapPointer(isolate, elements());

  CHECK_IMPLIES(HasSloppyArgumentsElements(), IsJSArgumentsObject(*this));
  if (HasFastProperties()) {
    int actual_unused_property_fields = map()->GetInObjectProperties() +
                                        property_array()->length() -
                                        map()->NextFreePropertyIndex();
    if (map()->UnusedPropertyFields() != actual_unused_property_fields) {
      // There are two reasons why this can happen:
      // - in the middle of StoreTransitionStub when the new extended backing
      //   store is already set into the object and the allocation of the
      //   HeapNumber triggers GC while the map isn't updated yet.
      // - deletion of the last property can leave additional backing store
      //   capacity behind.
      CHECK_GT(actual_unused_property_fields, map()->UnusedPropertyFields());
      int delta = actual_unused_property_fields - map()->UnusedPropertyFields();
      CHECK_EQ(0, delta % JSObject::kFieldsAdded);
    }
    DescriptorArray descriptors = map()->instance_descriptors(isolate);
    bool is_transitionable_fast_elements_kind =
        IsTransitionableFastElementsKind(map()->elements_kind());

    for (InternalIndex i : map()->IterateOwnDescriptors()) {
      PropertyDetails details = descriptors->GetDetails(i);
      if (details.location() == PropertyLocation::kField) {
        DCHECK_EQ(PropertyKind::kData, details.kind());
        Representation r = details.representation();
        FieldIndex index = FieldIndex::ForDetails(map(), details);
        if (COMPRESS_POINTERS_BOOL && index.is_inobject()) {
          VerifyObjectField(isolate, index.offset());
        }
        Object value = RawFastPropertyAt(index);
        if (r.IsDouble()) DCHECK(IsHeapNumber(value));
        if (IsUninitialized(value, isolate)) continue;
        if (r.IsSmi()) DCHECK(IsSmi(value));
        if (r.IsHeapObject()) DCHECK(IsHeapObject(value));
        FieldType field_type = descriptors->GetFieldType(i);
        bool type_is_none = IsNone(field_type);
        bool type_is_any = IsAny(field_type);
        if (r.IsNone()) {
          CHECK(type_is_none);
        } else if (!type_is_any && !(type_is_none && r.IsHeapObject())) {
          CHECK(!field_type.NowStable() || field_type.NowContains(value));
        }
        CHECK_IMPLIES(is_transitionable_fast_elements_kind,
                      Map::IsMostGeneralFieldType(r, field_type));
      }
    }

    if (map()->EnumLength() != kInvalidEnumCacheSentinel) {
      EnumCache enum_cache = descriptors->enum_cache();
      FixedArray keys = enum_cache->keys();
      FixedArray indices = enum_cache->indices();
      CHECK_LE(map()->EnumLength(), keys->length());
      CHECK_IMPLIES(indices != ReadOnlyRoots(isolate).empty_fixed_array(),
                    keys->length() == indices->length());
    }
  }

  // If a GC was caused while constructing this object, the elements
  // pointer may point to a one pointer filler map.
  if (ElementsAreSafeToExamine(isolate)) {
    CHECK_EQ((map()->has_fast_smi_or_object_elements() ||
              map()->has_any_nonextensible_elements() ||
              (elements() == GetReadOnlyRoots().empty_fixed_array()) ||
              HasFastStringWrapperElements()),
             (elements()->map() == GetReadOnlyRoots().fixed_array_map() ||
              elements()->map() == GetReadOnlyRoots().fixed_cow_array_map()));
    CHECK_EQ(map()->has_fast_object_elements(), HasObjectElements());
    VerifyJSObjectElements(isolate, *this);
  }
}

void Map::MapVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::MapVerify(*this, isolate);
  Heap* heap = isolate->heap();
  CHECK(!ObjectInYoungGeneration(*this));
  CHECK(FIRST_TYPE <= instance_type() && instance_type() <= LAST_TYPE);
  CHECK(instance_size() == kVariableSizeSentinel ||
        (kTaggedSize <= instance_size() &&
         static_cast<size_t>(instance_size()) < heap->Capacity()));
  if (IsContextMap(*this)) {
    // The map for the NativeContext is allocated before the NativeContext
    // itself, so it may happen that during a GC the native_context() is still
    // null.
    CHECK(IsNull(native_context_or_null()) ||
          IsNativeContext(native_context()));
  } else {
    if (IsUndefined(GetBackPointer(), isolate)) {
      // Root maps must not have descriptors in the descriptor array that do not
      // belong to the map.
      CHECK_EQ(NumberOfOwnDescriptors(),
               instance_descriptors(isolate)->number_of_descriptors());
    } else {
      // If there is a parent map it must be non-stable.
      Map parent = Map::cast(GetBackPointer());
      CHECK(!parent->is_stable());
      DescriptorArray descriptors = instance_descriptors(isolate);
      if (descriptors == parent->instance_descriptors(isolate)) {
        if (NumberOfOwnDescriptors() == parent->NumberOfOwnDescriptors() + 1) {
          // Descriptors sharing through property transitions takes over
          // ownership from the parent map.
          CHECK(!parent->owns_descriptors());
        } else {
          CHECK_EQ(NumberOfOwnDescriptors(), parent->NumberOfOwnDescriptors());
          // Descriptors sharing through special transitions properly takes over
          // ownership from the parent map unless it uses the canonical empty
          // descriptor array.
          if (descriptors != ReadOnlyRoots(isolate).empty_descriptor_array()) {
            CHECK_IMPLIES(owns_descriptors(), !parent->owns_descriptors());
            CHECK_IMPLIES(parent->owns_descriptors(), !owns_descriptors());
          }
        }
      }
    }
  }
  SLOW_DCHECK(instance_descriptors(isolate)->IsSortedNoDuplicates());
  SLOW_DCHECK(TransitionsAccessor(isolate, *this).IsSortedNoDuplicates());
  SLOW_DCHECK(
      TransitionsAccessor(isolate, *this).IsConsistentWithBackPointers());
  // Only JSFunction maps have has_prototype_slot() bit set and constructible
  // JSFunction objects must have prototype slot.
  CHECK_IMPLIES(has_prototype_slot(), IsJSFunctionMap(*this));

  if (IsJSObjectMap(*this)) {
    int header_end_offset = JSObject::GetHeaderSize(*this);
    int inobject_fields_start_offset = GetInObjectPropertyOffset(0);
    // Ensure that embedder fields are located exactly between header and
    // inobject properties.
    CHECK_EQ(header_end_offset, JSObject::GetEmbedderFieldsStartOffset(*this));
    CHECK_EQ(header_end_offset +
                 JSObject::GetEmbedderFieldCount(*this) * kEmbedderDataSlotSize,
             inobject_fields_start_offset);

    if (IsJSSharedStructMap(*this) || IsJSSharedArrayMap(*this) ||
        IsJSAtomicsMutex(*this) || IsJSAtomicsCondition(*this)) {
      if (COMPRESS_POINTERS_IN_ISOLATE_CAGE_BOOL) {
        // TODO(v8:14089): Verify what should be checked in this configuration
        // and again merge with the else-branch below.
        // CHECK(InSharedHeap());
        CHECK(IsUndefined(GetBackPointer(), isolate));
        // Object maybe_cell = prototype_validity_cell(kRelaxedLoad);
        // if (maybe_cell.IsCell()) CHECK(maybe_cell.InSharedHeap());
        CHECK(!is_extensible());
        CHECK(!is_prototype_map());
        CHECK(OnlyHasSimpleProperties());
        // CHECK(instance_descriptors(isolate).InSharedHeap());
        if (IsJSSharedArrayMap(*this)) {
          CHECK(has_shared_array_elements());
        }
      } else {
        CHECK(Object::InSharedHeap(*this));
        CHECK(IsUndefined(GetBackPointer(), isolate));
        Object maybe_cell = prototype_validity_cell(kRelaxedLoad);
        if (IsCell(maybe_cell)) CHECK(Object::InSharedHeap(maybe_cell));
        CHECK(!is_extensible());
        CHECK(!is_prototype_map());
        CHECK(OnlyHasSimpleProperties());
        CHECK(Object::InSharedHeap(instance_descriptors(isolate)));
        if (IsJSSharedArrayMap(*this)) {
          CHECK(has_shared_array_elements());
        }
      }
    }

    // Check constuctor value in JSFunction's maps.
    if (IsJSFunctionMap(*this) && !IsMap(constructor_or_back_pointer())) {
      Object maybe_constructor = constructor_or_back_pointer();
      // Constructor field might still contain a tuple if this map used to
      // have non-instance prototype earlier.
      CHECK_IMPLIES(has_non_instance_prototype(), IsTuple2(maybe_constructor));
      if (IsTuple2(maybe_constructor)) {
        Tuple2 tuple = Tuple2::cast(maybe_constructor);
        // Unwrap the {constructor, non-instance_prototype} pair.
        maybe_constructor = tuple->value1();
        CHECK(!IsJSReceiver(tuple->value2()));
      }
      CHECK(IsJSFunction(maybe_constructor) ||
            IsFunctionTemplateInfo(maybe_constructor) ||
            // The above check might fail until empty function setup is done.
            IsUndefined(isolate->raw_native_context()->get(
                Context::EMPTY_FUNCTION_INDEX)));
    }
  }

  if (!may_have_interesting_properties()) {
    CHECK(!has_named_interceptor());
    CHECK(!is_dictionary_map());
    CHECK(!is_access_check_needed());
    DescriptorArray const descriptors = instance_descriptors(isolate);
    for (InternalIndex i : IterateOwnDescriptors()) {
      CHECK(!descriptors->GetKey(i)->IsInteresting(isolate));
    }
  }
  CHECK_IMPLIES(has_named_interceptor(), may_have_interesting_properties());
  CHECK_IMPLIES(is_dictionary_map(), may_have_interesting_properties());
  CHECK_IMPLIES(is_access_check_needed(), may_have_interesting_properties());
  CHECK_IMPLIES(
      IsJSObjectMap(*this) && !CanHaveFastTransitionableElementsKind(),
      IsDictionaryElementsKind(elements_kind()) ||
          IsTerminalElementsKind(elements_kind()) ||
          IsAnyHoleyNonextensibleElementsKind(elements_kind()) ||
          IsSharedArrayElementsKind(elements_kind()));
  CHECK_IMPLIES(is_deprecated(), !is_stable());
  if (is_prototype_map()) {
    CHECK(prototype_info() == Smi::zero() || IsPrototypeInfo(prototype_info()));
  }
}

void Map::DictionaryMapVerify(Isolate* isolate) {
  MapVerify(isolate);
  CHECK(is_dictionary_map());
  CHECK_EQ(kInvalidEnumCacheSentinel, EnumLength());
  CHECK_EQ(ReadOnlyRoots(isolate).empty_descriptor_array(),
           instance_descriptors(isolate));
  CHECK_EQ(0, UnusedPropertyFields());
  CHECK_EQ(Map::GetVisitorId(*this), visitor_id());
}

void EmbedderDataArray::EmbedderDataArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::EmbedderDataArrayVerify(*this, isolate);
  EmbedderDataSlot start(*this, 0);
  EmbedderDataSlot end(*this, length());
  for (EmbedderDataSlot slot = start; slot < end; ++slot) {
    Object e = slot.load_tagged();
    Object::VerifyPointer(isolate, e);
  }
}

void FixedArray::FixedArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::FixedArrayVerify(*this, isolate);
  if (*this == ReadOnlyRoots(isolate).empty_fixed_array()) {
    CHECK_EQ(length(), 0);
    CHECK_EQ(map(), ReadOnlyRoots(isolate).fixed_array_map());
  } else if (IsArrayList(*this)) {
    ArrayList::cast(*this)->ArrayListVerify(isolate);
  }
}

void WeakFixedArray::WeakFixedArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::WeakFixedArrayVerify(*this, isolate);
  for (int i = 0; i < length(); i++) {
    MaybeObject::VerifyMaybeObjectPointer(isolate, Get(i));
  }
}

void ArrayList::ArrayListVerify(Isolate* isolate) {
  // Avoid calling the torque-generated ArrayListVerify to prevent an endlessly
  // recursion verification.
  CHECK(IsArrayList(*this));
  CHECK_LE(ArrayList::kLengthIndex, length());
  CHECK_LE(0, Length());
  if (Length() == 0 && length() == ArrayList::kLengthIndex) {
    CHECK_EQ(*this, ReadOnlyRoots(isolate).empty_array_list());
  }
}

void PropertyArray::PropertyArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::PropertyArrayVerify(*this, isolate);
  if (length() == 0) {
    CHECK_EQ(*this, ReadOnlyRoots(isolate).empty_property_array());
    return;
  }
  // There are no empty PropertyArrays.
  CHECK_LT(0, length());
  for (int i = 0; i < length(); i++) {
    Object e = get(i);
    Object::VerifyPointer(isolate, e);
  }
}

void FixedDoubleArray::FixedDoubleArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::FixedDoubleArrayVerify(*this, isolate);
  for (int i = 0; i < length(); i++) {
    if (!is_the_hole(i)) {
      uint64_t value = get_representation(i);
      uint64_t unexpected =
          base::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN()) &
          uint64_t{0x7FF8000000000000};
      // Create implementation specific sNaN by inverting relevant bit.
      unexpected ^= uint64_t{0x0008000000000000};
      CHECK((value & uint64_t{0x7FF8000000000000}) != unexpected ||
            (value & uint64_t{0x0007FFFFFFFFFFFF}) == uint64_t{0});
    }
  }
}

void Context::ContextVerify(Isolate* isolate) {
  if (has_extension()) VerifyExtensionSlot(extension());
  TorqueGeneratedClassVerifiers::ContextVerify(*this, isolate);
  for (int i = 0; i < length(); i++) {
    VerifyObjectField(isolate, OffsetOfElementAt(i));
  }
}

void NativeContext::NativeContextVerify(Isolate* isolate) {
  ContextVerify(isolate);
  CHECK(retained_maps() == Smi::zero() || IsWeakArrayList(retained_maps()));
  CHECK_EQ(length(), NativeContext::NATIVE_CONTEXT_SLOTS);
  CHECK_EQ(kVariableSizeSentinel, map()->instance_size());
}

void FeedbackMetadata::FeedbackMetadataVerify(Isolate* isolate) {
  if (slot_count() == 0 && create_closure_slot_count() == 0) {
    CHECK_EQ(ReadOnlyRoots(isolate).empty_feedback_metadata(), *this);
  } else {
    FeedbackMetadataIterator iter(*this);
    while (iter.HasNext()) {
      iter.Next();
      FeedbackSlotKind kind = iter.kind();
      CHECK_NE(FeedbackSlotKind::kInvalid, kind);
      CHECK_GT(kFeedbackSlotKindCount, kind);
    }
  }
}

void DescriptorArray::DescriptorArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::DescriptorArrayVerify(*this, isolate);
  if (number_of_all_descriptors() == 0) {
    CHECK_EQ(ReadOnlyRoots(isolate).empty_descriptor_array(), *this);
    CHECK_EQ(0, number_of_all_descriptors());
    CHECK_EQ(0, number_of_descriptors());
    CHECK_EQ(ReadOnlyRoots(isolate).empty_enum_cache(), enum_cache());
  } else {
    CHECK_LT(0, number_of_all_descriptors());
    CHECK_LE(number_of_descriptors(), number_of_all_descriptors());

    // Check that properties with private symbols names are non-enumerable, and
    // that fields are in order.
    int expected_field_index = 0;
    for (InternalIndex descriptor :
         InternalIndex::Range(number_of_descriptors())) {
      Object key = *(GetDescriptorSlot(descriptor.as_int()) + kEntryKeyIndex);
      // number_of_descriptors() may be out of sync with the actual descriptors
      // written during descriptor array construction.
      if (IsUndefined(key, isolate)) continue;
      PropertyDetails details = GetDetails(descriptor);
      if (Name::cast(key)->IsPrivate()) {
        CHECK_NE(details.attributes() & DONT_ENUM, 0);
      }
      MaybeObject value = GetValue(descriptor);
      HeapObject heap_object;
      if (details.location() == PropertyLocation::kField) {
        CHECK_EQ(details.field_index(), expected_field_index);
        CHECK(value == MaybeObject::FromObject(FieldType::None()) ||
              value == MaybeObject::FromObject(FieldType::Any()) ||
              value->IsCleared() ||
              (value->GetHeapObjectIfWeak(&heap_object) && IsMap(heap_object)));
        expected_field_index += details.field_width_in_words();
      } else {
        CHECK(!value->IsWeakOrCleared());
        CHECK(!IsMap(value->cast<Object>()));
      }
    }
  }
}

void TransitionArray::TransitionArrayVerify(Isolate* isolate) {
  WeakFixedArrayVerify(isolate);
  CHECK_LE(LengthFor(number_of_transitions()), length());
}

namespace {
void SloppyArgumentsElementsVerify(Isolate* isolate,
                                   SloppyArgumentsElements elements,
                                   JSObject holder) {
  elements->SloppyArgumentsElementsVerify(isolate);
  ElementsKind kind = holder->GetElementsKind();
  bool is_fast = kind == FAST_SLOPPY_ARGUMENTS_ELEMENTS;
  Context context_object = elements->context();
  FixedArray arg_elements = elements->arguments();
  if (arg_elements->length() == 0) {
    CHECK(arg_elements == ReadOnlyRoots(isolate).empty_fixed_array());
    return;
  }
  ElementsAccessor* accessor;
  if (is_fast) {
    accessor = ElementsAccessor::ForKind(HOLEY_ELEMENTS);
  } else {
    accessor = ElementsAccessor::ForKind(DICTIONARY_ELEMENTS);
  }
  int nofMappedParameters = 0;
  int maxMappedIndex = 0;
  for (int i = 0; i < nofMappedParameters; i++) {
    // Verify that each context-mapped argument is either the hole or a valid
    // Smi within context length range.
    Object mapped = elements->mapped_entries(i, kRelaxedLoad);
    if (IsTheHole(mapped, isolate)) {
      // Slow sloppy arguments can be holey.
      if (!is_fast) continue;
      // Fast sloppy arguments elements are never holey. Either the element is
      // context-mapped or present in the arguments elements.
      CHECK(accessor->HasElement(holder, i, arg_elements));
      continue;
    }
    int mappedIndex = Smi::ToInt(mapped);
    nofMappedParameters++;
    CHECK_LE(maxMappedIndex, mappedIndex);
    maxMappedIndex = mappedIndex;
    Object value = context_object->get(mappedIndex);
    CHECK(IsObject(value));
    // None of the context-mapped entries should exist in the arguments
    // elements.
    CHECK(!accessor->HasElement(holder, i, arg_elements));
  }
  CHECK_LE(nofMappedParameters, context_object->length());
  CHECK_LE(nofMappedParameters, arg_elements->length());
  CHECK_LE(maxMappedIndex, context_object->length());
  CHECK_LE(maxMappedIndex, arg_elements->length());
}
}  // namespace

void JSArgumentsObject::JSArgumentsObjectVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSArgumentsObjectVerify(*this, isolate);
  if (IsSloppyArgumentsElementsKind(GetElementsKind())) {
    SloppyArgumentsElementsVerify(
        isolate, SloppyArgumentsElements::cast(elements()), *this);
  }
  if (isolate->IsInAnyContext(map(), Context::SLOPPY_ARGUMENTS_MAP_INDEX) ||
      isolate->IsInAnyContext(map(),
                              Context::SLOW_ALIASED_ARGUMENTS_MAP_INDEX) ||
      isolate->IsInAnyContext(map(),
                              Context::FAST_ALIASED_ARGUMENTS_MAP_INDEX)) {
    VerifyObjectField(isolate, JSSloppyArgumentsObject::kLengthOffset);
    VerifyObjectField(isolate, JSSloppyArgumentsObject::kCalleeOffset);
  } else if (isolate->IsInAnyContext(map(),
                                     Context::STRICT_ARGUMENTS_MAP_INDEX)) {
    VerifyObjectField(isolate, JSStrictArgumentsObject::kLengthOffset);
  }
}

void JSAsyncFunctionObject::JSAsyncFunctionObjectVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSAsyncFunctionObjectVerify(*this, isolate);
}

void JSAsyncGeneratorObject::JSAsyncGeneratorObjectVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSAsyncGeneratorObjectVerify(*this, isolate);
}

void JSDate::JSDateVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSDateVerify(*this, isolate);

  if (IsSmi(month())) {
    int month = Smi::ToInt(this->month());
    CHECK(0 <= month && month <= 11);
  }
  if (IsSmi(day())) {
    int day = Smi::ToInt(this->day());
    CHECK(1 <= day && day <= 31);
  }
  if (IsSmi(hour())) {
    int hour = Smi::ToInt(this->hour());
    CHECK(0 <= hour && hour <= 23);
  }
  if (IsSmi(min())) {
    int min = Smi::ToInt(this->min());
    CHECK(0 <= min && min <= 59);
  }
  if (IsSmi(sec())) {
    int sec = Smi::ToInt(this->sec());
    CHECK(0 <= sec && sec <= 59);
  }
  if (IsSmi(weekday())) {
    int weekday = Smi::ToInt(this->weekday());
    CHECK(0 <= weekday && weekday <= 6);
  }
  if (IsSmi(cache_stamp())) {
    CHECK(Smi::ToInt(cache_stamp()) <=
          Smi::ToInt(isolate->date_cache()->stamp()));
  }
}

void String::StringVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::StringVerify(*this, isolate);
  CHECK(length() >= 0 && length() <= Smi::kMaxValue);
  CHECK_IMPLIES(length() == 0, *this == ReadOnlyRoots(isolate).empty_string());
  if (IsInternalizedString(*this)) {
    CHECK(HasHashCode());
    CHECK(!ObjectInYoungGeneration(*this));
  }
}

void ConsString::ConsStringVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ConsStringVerify(*this, isolate);
  CHECK_GE(length(), ConsString::kMinLength);
  CHECK(length() == first()->length() + second()->length());
  if (IsFlat(isolate)) {
    // A flat cons can only be created by String::SlowFlatten.
    // Afterwards, the first part may be externalized or internalized.
    CHECK(IsSeqString(first()) || IsExternalString(first()) ||
          IsThinString(first()));
  }
}

void ThinString::ThinStringVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ThinStringVerify(*this, isolate);
  CHECK(!HasForwardingIndex(kAcquireLoad));
  CHECK(IsInternalizedString(actual()));
  CHECK(IsSeqString(actual()) || IsExternalString(actual()));
}

void SlicedString::SlicedStringVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::SlicedStringVerify(*this, isolate);
  CHECK(!IsConsString(parent()));
  CHECK(!IsSlicedString(parent()));
#ifdef DEBUG
  if (!isolate->has_turbofan_string_builders()) {
    // Turbofan's string builder optimization can introduce SlicedString that
    // are less than SlicedString::kMinLength characters. Their live range and
    // scope are pretty limitted, but they can be visible to the GC, which
    // shouldn't treat them as invalid.
    CHECK_GE(length(), SlicedString::kMinLength);
  }
#endif
}

USE_TORQUE_VERIFIER(ExternalString)

void JSBoundFunction::JSBoundFunctionVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSBoundFunctionVerify(*this, isolate);
  CHECK(IsCallable(*this));
  CHECK_EQ(IsConstructor(*this), IsConstructor(bound_target_function()));
}

void JSFunction::JSFunctionVerify(Isolate* isolate) {
  // Don't call TorqueGeneratedClassVerifiers::JSFunctionVerify here because the
  // Torque class definition contains the field `prototype_or_initial_map` which
  // may not be allocated.

  // This assertion exists to encourage updating this verification function if
  // new fields are added in the Torque class layout definition.
  static_assert(JSFunction::TorqueGeneratedClass::kHeaderSize ==
                8 * kTaggedSize);

  JSFunctionOrBoundFunctionOrWrappedFunctionVerify(isolate);
  CHECK(IsJSFunction(*this));
  VerifyPointer(isolate, shared(isolate));
  CHECK(IsSharedFunctionInfo(shared(isolate)));
  VerifyPointer(isolate, context(isolate, kRelaxedLoad));
  CHECK(IsContext(context(isolate, kRelaxedLoad)));
  VerifyPointer(isolate, raw_feedback_cell(isolate));
  CHECK(IsFeedbackCell(raw_feedback_cell(isolate)));
  VerifyPointer(isolate, code(isolate));
  CHECK(IsCode(code(isolate)));
  CHECK(map(isolate)->is_callable());
  Handle<JSFunction> function(*this, isolate);
  LookupIterator it(isolate, function, isolate->factory()->prototype_string(),
                    LookupIterator::OWN_SKIP_INTERCEPTOR);
  if (has_prototype_slot()) {
    VerifyObjectField(isolate, kPrototypeOrInitialMapOffset);
  }

  if (has_prototype_property()) {
    CHECK(it.IsFound());
    CHECK_EQ(LookupIterator::ACCESSOR, it.state());
    CHECK(IsAccessorInfo(*it.GetAccessors()));
  } else {
    CHECK(!it.IsFound() || it.state() != LookupIterator::ACCESSOR ||
          !IsAccessorInfo(*it.GetAccessors()));
  }
}

void SharedFunctionInfo::SharedFunctionInfoVerify(Isolate* isolate) {
  // TODO(leszeks): Add a TorqueGeneratedClassVerifier for LocalIsolate.
  SharedFunctionInfoVerify(ReadOnlyRoots(isolate));
}

void SharedFunctionInfo::SharedFunctionInfoVerify(LocalIsolate* isolate) {
  SharedFunctionInfoVerify(ReadOnlyRoots(isolate));
}

namespace {

bool ShouldVerifySharedFunctionInfoFunctionIndex(SharedFunctionInfo sfi) {
  if (!sfi->HasBuiltinId()) return true;
  switch (sfi->builtin_id()) {
    case Builtin::kPromiseCapabilityDefaultReject:
    case Builtin::kPromiseCapabilityDefaultResolve:
      // For these we manually set custom function indices.
      return false;
    default:
      return true;
  }
  UNREACHABLE();
}

}  // namespace

void SharedFunctionInfo::SharedFunctionInfoVerify(ReadOnlyRoots roots) {
  Object value = name_or_scope_info(kAcquireLoad);
  if (IsScopeInfo(value)) {
    CHECK(!ScopeInfo::cast(value)->IsEmpty());
    CHECK_NE(value, roots.empty_scope_info());
  }

#if V8_ENABLE_WEBASSEMBLY
  bool is_wasm = HasWasmExportedFunctionData() || HasAsmWasmData() ||
                 HasWasmJSFunctionData() || HasWasmCapiFunctionData() ||
                 HasWasmResumeData();
#else
  bool is_wasm = false;
#endif  // V8_ENABLE_WEBASSEMBLY
  CHECK(is_wasm || IsApiFunction() || HasBytecodeArray() || HasBuiltinId() ||
        HasUncompiledDataWithPreparseData() ||
        HasUncompiledDataWithoutPreparseData());

  {
    HeapObject script = this->script(kAcquireLoad);
    CHECK(IsUndefined(script, roots) || IsScript(script));
  }

  if (!is_compiled()) {
    CHECK(!HasFeedbackMetadata());
    CHECK(IsScopeInfo(outer_scope_info()) ||
          IsTheHole(outer_scope_info(), roots));
  } else if (HasBytecodeArray() && HasFeedbackMetadata()) {
    CHECK(IsFeedbackMetadata(feedback_metadata()));
  }

  if (ShouldVerifySharedFunctionInfoFunctionIndex(*this)) {
    int expected_map_index =
        Context::FunctionMapIndex(language_mode(), kind(), HasSharedName());
    CHECK_EQ(expected_map_index, function_map_index());
  }

  ScopeInfo info = EarlyScopeInfo(kAcquireLoad);
  if (!info->IsEmpty()) {
    CHECK(kind() == info->function_kind());
    CHECK_EQ(internal::IsModule(kind()), info->scope_type() == MODULE_SCOPE);
  }

  if (IsApiFunction()) {
    CHECK(construct_as_builtin());
  } else if (!HasBuiltinId()) {
    CHECK(!construct_as_builtin());
  } else {
    if (builtin_id() != Builtin::kCompileLazy &&
        builtin_id() != Builtin::kEmptyFunction) {
      CHECK(construct_as_builtin());
    } else {
      CHECK(!construct_as_builtin());
    }
  }
}

void JSGlobalProxy::JSGlobalProxyVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSGlobalProxyVerify(*this, isolate);
  CHECK(map()->is_access_check_needed());
  // Make sure that this object has no properties, elements.
  CHECK_EQ(0, FixedArray::cast(elements())->length());
}

void JSGlobalObject::JSGlobalObjectVerify(Isolate* isolate) {
  CHECK(IsJSGlobalObject(*this));
  // Do not check the dummy global object for the builtins.
  if (global_dictionary(kAcquireLoad)->NumberOfElements() == 0 &&
      elements()->length() == 0) {
    return;
  }
  JSObjectVerify(isolate);
}

void Oddball::OddballVerify(Isolate* isolate) {
  PrimitiveHeapObjectVerify(isolate);
  CHECK(IsOddball(*this, isolate));

  Heap* heap = isolate->heap();
  Object string = to_string();
  VerifyPointer(isolate, string);
  CHECK(IsString(string));
  Object type = type_of();
  VerifyPointer(isolate, type);
  CHECK(IsString(type));
  Object kind_value = TaggedField<Object>::load(*this, kKindOffset);
  VerifyPointer(isolate, kind_value);
  CHECK(IsSmi(kind_value));

  Object number = to_number();
  VerifyPointer(isolate, number);
  CHECK(IsSmi(number) || IsHeapNumber(number));
  if (IsHeapObject(number)) {
    CHECK(number == ReadOnlyRoots(heap).nan_value() ||
          number == ReadOnlyRoots(heap).hole_nan_value());
  } else {
    CHECK(IsSmi(number));
    int value = Smi::ToInt(number);
    // Hidden oddballs have negative smis.
    const int kLeastHiddenOddballNumber = -7;
    CHECK_LE(value, 1);
    CHECK_GE(value, kLeastHiddenOddballNumber);
  }

  ReadOnlyRoots roots(heap);
  if (map() == roots.undefined_map()) {
    CHECK(*this == roots.undefined_value());
  } else if (map() == roots.null_map()) {
    CHECK(*this == roots.null_value());
  } else if (map() == roots.boolean_map()) {
    CHECK(*this == roots.true_value() || *this == roots.false_value());
  } else if (map() == roots.uninitialized_map()) {
    CHECK(*this == roots.uninitialized_value());
  } else if (map() == roots.arguments_marker_map()) {
    CHECK(*this == roots.arguments_marker());
  } else if (map() == roots.termination_exception_map()) {
    CHECK(*this == roots.termination_exception());
  } else if (map() == roots.exception_map()) {
    CHECK(*this == roots.exception());
  } else if (map() == roots.optimized_out_map()) {
    CHECK(*this == roots.optimized_out());
  } else if (map() == roots.stale_register_map()) {
    CHECK(*this == roots.stale_register());
  } else if (map() == roots.self_reference_marker_map()) {
    // Multiple instances of this oddball may exist at once.
    CHECK_EQ(kind(), Oddball::kSelfReferenceMarker);
  } else if (map() == roots.basic_block_counters_marker_map()) {
    CHECK(*this == roots.basic_block_counters_marker());
  } else {
    UNREACHABLE();
  }
}

void Hole::HoleVerify(Isolate* isolate) {
  ReadOnlyRoots roots(isolate->heap());
  CHECK_EQ(map(), roots.hole_map());

#define COMPARE_ROOTS_VALUE(_, Value, __) \
  if (*this == roots.Value()) {           \
    return;                               \
  }
  HOLE_LIST(COMPARE_ROOTS_VALUE);
#undef COMPARE_ROOTS_VALUE

  UNREACHABLE();
}

void PropertyCell::PropertyCellVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::PropertyCellVerify(*this, isolate);
  CHECK(IsUniqueName(name()));
  CheckDataIsCompatible(property_details(), value());
}

void Code::CodeVerify(Isolate* isolate) {
  CHECK(IsCode(*this));
  if (has_instruction_stream()) {
    InstructionStream istream = instruction_stream();
    CHECK_EQ(istream->code(kAcquireLoad), *this);
    CHECK_EQ(safepoint_table_offset(), 0);
    CHECK_LE(safepoint_table_offset(), handler_table_offset());
    CHECK_LE(handler_table_offset(), constant_pool_offset());
    CHECK_LE(constant_pool_offset(), code_comments_offset());
    CHECK_LE(code_comments_offset(), unwinding_info_offset());
    CHECK_LE(unwinding_info_offset(), metadata_size());

    // Ensure the cached code entry point corresponds to the InstructionStream
    // object associated with this Code.
#if defined(V8_COMPRESS_POINTERS) && defined(V8_SHORT_BUILTIN_CALLS)
    if (istream->instruction_start() == instruction_start()) {
      // Most common case, all good.
    } else {
      // When shared pointer compression cage is enabled and it has the
      // embedded code blob copy then the
      // InstructionStream::instruction_start() might return the address of
      // the remapped builtin regardless of whether the builtins copy existed
      // when the instruction_start value was cached in the Code (see
      // InstructionStream::OffHeapInstructionStart()).  So, do a reverse
      // Code object lookup via instruction_start value to ensure it
      // corresponds to this current Code object.
      Code lookup_result =
          isolate->heap()->FindCodeForInnerPointer(instruction_start());
      CHECK_EQ(lookup_result, *this);
    }
#else
    CHECK_EQ(istream.instruction_start(), instruction_start());
#endif  // V8_COMPRESS_POINTERS && V8_SHORT_BUILTIN_CALLS
  }

#if defined(V8_CODE_POINTER_SANDBOXING)
  // Check that the code pointer table entry is consistent, i.e. points back to
  // this Code object.
  CHECK_EQ(ReadIndirectPointerField(kCodePointerTableEntryOffset), *this);
#endif
}

void InstructionStream::InstructionStreamVerify(Isolate* isolate) {
  Code code;
  if (!TryGetCode(&code, kAcquireLoad)) return;
  CHECK(
      IsAligned(code->instruction_size(),
                static_cast<unsigned>(InstructionStream::kMetadataAlignment)));
#if !defined(_MSC_VER) || defined(__clang__)
  // See also: PlatformEmbeddedFileWriterWin::AlignToCodeAlignment.
  CHECK_IMPLIES(!ReadOnlyHeap::Contains(*this),
                IsAligned(instruction_start(), kCodeAlignment));
#endif  // !defined(_MSC_VER) || defined(__clang__)
  CHECK_IMPLIES(!ReadOnlyHeap::Contains(*this),
                IsAligned(instruction_start(), kCodeAlignment));
  CHECK_EQ(*this, code->instruction_stream());
  CHECK(V8_ENABLE_THIRD_PARTY_HEAP_BOOL ||
        Size() <= MemoryChunkLayout::MaxRegularCodeObjectSize() ||
        isolate->heap()->InSpace(*this, CODE_LO_SPACE));
  Address last_gc_pc = kNullAddress;

  Object::ObjectVerify(relocation_info(), isolate);

  for (RelocIterator it(code); !it.done(); it.next()) {
    it.rinfo()->Verify(isolate);
    // Ensure that GC will not iterate twice over the same pointer.
    if (RelocInfo::IsGCRelocMode(it.rinfo()->rmode())) {
      CHECK(it.rinfo()->pc() != last_gc_pc);
      last_gc_pc = it.rinfo()->pc();
    }
  }
}

void JSArray::JSArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSArrayVerify(*this, isolate);
  // If a GC was caused while constructing this array, the elements
  // pointer may point to a one pointer filler map.
  if (!ElementsAreSafeToExamine(isolate)) return;
  if (IsUndefined(elements(), isolate)) return;
  CHECK(IsFixedArray(elements()) || IsFixedDoubleArray(elements()));
  if (elements()->length() == 0) {
    CHECK_EQ(elements(), ReadOnlyRoots(isolate).empty_fixed_array());
  }
  // Verify that the length and the elements backing store are in sync.
  if (IsSmi(length()) && (HasFastElements() || HasAnyNonextensibleElements())) {
    if (elements()->length() > 0) {
      CHECK_IMPLIES(HasDoubleElements(), IsFixedDoubleArray(elements()));
      CHECK_IMPLIES(HasSmiOrObjectElements() || HasAnyNonextensibleElements(),
                    IsFixedArray(elements()));
    }
    int size = Smi::ToInt(length());
    // Holey / Packed backing stores might have slack or might have not been
    // properly initialized yet.
    CHECK(size <= elements()->length() ||
          elements() == ReadOnlyRoots(isolate).empty_fixed_array());
  } else {
    CHECK(HasDictionaryElements());
    uint32_t array_length;
    CHECK(Object::ToArrayLength(length(), &array_length));
    if (array_length == 0xFFFFFFFF) {
      CHECK(Object::ToArrayLength(length(), &array_length));
    }
    if (array_length != 0) {
      NumberDictionary dict = NumberDictionary::cast(elements());
      // The dictionary can never have more elements than the array length + 1.
      // If the backing store grows the verification might be triggered with
      // the old length in place.
      uint32_t nof_elements = static_cast<uint32_t>(dict->NumberOfElements());
      if (nof_elements != 0) nof_elements--;
      CHECK_LE(nof_elements, array_length);
    }
  }
}

void JSSet::JSSetVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSSetVerify(*this, isolate);
  CHECK(IsOrderedHashSet(table()) || IsUndefined(table(), isolate));
  // TODO(arv): Verify OrderedHashTable too.
}

void JSMap::JSMapVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSMapVerify(*this, isolate);
  CHECK(IsOrderedHashMap(table()) || IsUndefined(table(), isolate));
  // TODO(arv): Verify OrderedHashTable too.
}

void JSSetIterator::JSSetIteratorVerify(Isolate* isolate) {
  CHECK(IsJSSetIterator(*this));
  JSCollectionIteratorVerify(isolate);
  CHECK(IsOrderedHashSet(table()));
  CHECK(IsSmi(index()));
}

void JSMapIterator::JSMapIteratorVerify(Isolate* isolate) {
  CHECK(IsJSMapIterator(*this));
  JSCollectionIteratorVerify(isolate);
  CHECK(IsOrderedHashMap(table()));
  CHECK(IsSmi(index()));
}

USE_TORQUE_VERIFIER(JSShadowRealm)
USE_TORQUE_VERIFIER(JSWrappedFunction)

namespace {

void VerifyElementIsShared(Object element) {
  // Exception for ThinStrings:
  // When storing a ThinString in a shared object, we want to store the actual
  // string, which is shared when sharing the string table.
  // It is possible that a stored shared string migrates to a ThinString later
  // on, which is fine as the ThinString resides in shared space if the original
  // string was in shared space.
  if (IsThinString(element)) {
    CHECK(v8_flags.shared_string_table);
    CHECK(Object::InWritableSharedSpace(element));
  } else {
    CHECK(IsShared(element));
  }
}

}  // namespace

void JSSharedStruct::JSSharedStructVerify(Isolate* isolate) {
  CHECK(IsJSSharedStruct(*this));
  CHECK(InWritableSharedSpace());
  JSObjectVerify(isolate);
  CHECK(HasFastProperties());
  // Shared structs can only point to primitives or other shared HeapObjects,
  // even internally.
  Map struct_map = map();
  CHECK(Object::InSharedHeap(property_array()));
  DescriptorArray descriptors = struct_map->instance_descriptors(isolate);
  for (InternalIndex i : struct_map->IterateOwnDescriptors()) {
    PropertyDetails details = descriptors->GetDetails(i);
    CHECK_EQ(PropertyKind::kData, details.kind());
    CHECK_EQ(PropertyLocation::kField, details.location());
    CHECK(details.representation().IsTagged());
    FieldIndex field_index = FieldIndex::ForDetails(struct_map, details);
    VerifyElementIsShared(RawFastPropertyAt(field_index));
  }
}

void JSAtomicsMutex::JSAtomicsMutexVerify(Isolate* isolate) {
  CHECK(IsJSAtomicsMutex(*this));
  CHECK(InWritableSharedSpace());
  JSObjectVerify(isolate);
}

void JSAtomicsCondition::JSAtomicsConditionVerify(Isolate* isolate) {
  CHECK(IsJSAtomicsCondition(*this));
  CHECK(Object::InSharedHeap(*this));
  JSObjectVerify(isolate);
}

void JSSharedArray::JSSharedArrayVerify(Isolate* isolate) {
  CHECK(IsJSSharedArray(*this));
  JSObjectVerify(isolate);
  CHECK(HasFastProperties());
  // Shared arrays can only point to primitives or other shared HeapObjects,
  // even internally.
  FixedArray storage = FixedArray::cast(elements());
  uint32_t length = storage->length();
  for (uint32_t j = 0; j < length; j++) {
    Object element_value = storage->get(j);
    VerifyElementIsShared(element_value);
  }
}

void JSIteratorMapHelper::JSIteratorMapHelperVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSIteratorMapHelperVerify(*this, isolate);
  CHECK(IsCallable(mapper()));
  CHECK_GE(Object::Number(counter()), 0);
}

void JSIteratorFilterHelper::JSIteratorFilterHelperVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSIteratorFilterHelperVerify(*this, isolate);
  CHECK(IsCallable(predicate()));
  CHECK_GE(Object::Number(counter()), 0);
}

void JSIteratorTakeHelper::JSIteratorTakeHelperVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSIteratorTakeHelperVerify(*this, isolate);
  CHECK_GE(Object::Number(remaining()), 0);
}

void JSIteratorDropHelper::JSIteratorDropHelperVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSIteratorDropHelperVerify(*this, isolate);
  CHECK_GE(Object::Number(remaining()), 0);
}

void JSIteratorFlatMapHelper::JSIteratorFlatMapHelperVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSIteratorFlatMapHelperVerify(*this, isolate);
  CHECK(IsCallable(mapper()));
  CHECK_GE(Object::Number(counter()), 0);
}

void WeakCell::WeakCellVerify(Isolate* isolate) {
  CHECK(IsWeakCell(*this));

  CHECK(IsUndefined(target(), isolate) || Object::CanBeHeldWeakly(target()));

  CHECK(IsWeakCell(prev()) || IsUndefined(prev(), isolate));
  if (IsWeakCell(prev())) {
    CHECK_EQ(WeakCell::cast(prev())->next(), *this);
  }

  CHECK(IsWeakCell(next()) || IsUndefined(next(), isolate));
  if (IsWeakCell(next())) {
    CHECK_EQ(WeakCell::cast(next())->prev(), *this);
  }

  CHECK_IMPLIES(IsUndefined(unregister_token(), isolate),
                IsUndefined(key_list_prev(), isolate));
  CHECK_IMPLIES(IsUndefined(unregister_token(), isolate),
                IsUndefined(key_list_next(), isolate));

  CHECK(IsWeakCell(key_list_prev()) || IsUndefined(key_list_prev(), isolate));

  CHECK(IsWeakCell(key_list_next()) || IsUndefined(key_list_next(), isolate));

  CHECK(IsUndefined(finalization_registry(), isolate) ||
        IsJSFinalizationRegistry(finalization_registry()));
}

void JSWeakRef::JSWeakRefVerify(Isolate* isolate) {
  CHECK(IsJSWeakRef(*this));
  JSObjectVerify(isolate);
  CHECK(IsUndefined(target(), isolate) || Object::CanBeHeldWeakly(target()));
}

void JSFinalizationRegistry::JSFinalizationRegistryVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSFinalizationRegistryVerify(*this, isolate);
  if (IsWeakCell(active_cells())) {
    CHECK(IsUndefined(WeakCell::cast(active_cells())->prev(), isolate));
  }
  if (IsWeakCell(cleared_cells())) {
    CHECK(IsUndefined(WeakCell::cast(cleared_cells())->prev(), isolate));
  }
}

void JSWeakMap::JSWeakMapVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSWeakMapVerify(*this, isolate);
  CHECK(IsEphemeronHashTable(table()) || IsUndefined(table(), isolate));
}

void JSArrayIterator::JSArrayIteratorVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSArrayIteratorVerify(*this, isolate);

  CHECK_GE(Object::Number(next_index()), 0);
  CHECK_LE(Object::Number(next_index()), kMaxSafeInteger);

  if (IsJSTypedArray(iterated_object())) {
    // JSTypedArray::length is limited to Smi range.
    CHECK(IsSmi(next_index()));
    CHECK_LE(Object::Number(next_index()), Smi::kMaxValue);
  } else if (IsJSArray(iterated_object())) {
    // JSArray::length is limited to Uint32 range.
    CHECK_LE(Object::Number(next_index()), kMaxUInt32);
  }
}

void JSStringIterator::JSStringIteratorVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSStringIteratorVerify(*this, isolate);
  CHECK_GE(index(), 0);
  CHECK_LE(index(), String::kMaxLength);
}

void JSWeakSet::JSWeakSetVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSWeakSetVerify(*this, isolate);
  CHECK(IsEphemeronHashTable(table()) || IsUndefined(table(), isolate));
}

void CallableTask::CallableTaskVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::CallableTaskVerify(*this, isolate);
  CHECK(IsCallable(callable()));
}

void JSPromise::JSPromiseVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSPromiseVerify(*this, isolate);
  if (status() == Promise::kPending) {
    CHECK(IsSmi(reactions()) || IsPromiseReaction(reactions()));
  }
}

template <typename Derived>
void SmallOrderedHashTable<Derived>::SmallOrderedHashTableVerify(
    Isolate* isolate) {
  CHECK(IsSmallOrderedHashTable(*this));

  int capacity = Capacity();
  CHECK_GE(capacity, kMinCapacity);
  CHECK_LE(capacity, kMaxCapacity);

  for (int entry = 0; entry < NumberOfBuckets(); entry++) {
    int bucket = GetFirstEntry(entry);
    if (bucket == kNotFound) continue;
    CHECK_GE(bucket, 0);
    CHECK_LE(bucket, capacity);
  }

  for (int entry = 0; entry < NumberOfElements(); entry++) {
    int chain = GetNextEntry(entry);
    if (chain == kNotFound) continue;
    CHECK_GE(chain, 0);
    CHECK_LE(chain, capacity);
  }

  for (int entry = 0; entry < NumberOfElements(); entry++) {
    for (int offset = 0; offset < Derived::kEntrySize; offset++) {
      Object val = GetDataEntry(entry, offset);
      VerifyPointer(isolate, val);
    }
  }

  for (int entry = NumberOfElements() + NumberOfDeletedElements();
       entry < Capacity(); entry++) {
    for (int offset = 0; offset < Derived::kEntrySize; offset++) {
      Object val = GetDataEntry(entry, offset);
      CHECK(IsTheHole(val, isolate));
    }
  }
}

void SmallOrderedHashMap::SmallOrderedHashMapVerify(Isolate* isolate) {
  CHECK(IsSmallOrderedHashMap(*this));
  SmallOrderedHashTable<SmallOrderedHashMap>::SmallOrderedHashTableVerify(
      isolate);
  for (int entry = NumberOfElements(); entry < NumberOfDeletedElements();
       entry++) {
    for (int offset = 0; offset < kEntrySize; offset++) {
      Object val = GetDataEntry(entry, offset);
      CHECK(IsTheHole(val, isolate));
    }
  }
}

void SmallOrderedHashSet::SmallOrderedHashSetVerify(Isolate* isolate) {
  CHECK(IsSmallOrderedHashSet(*this));
  SmallOrderedHashTable<SmallOrderedHashSet>::SmallOrderedHashTableVerify(
      isolate);
  for (int entry = NumberOfElements(); entry < NumberOfDeletedElements();
       entry++) {
    for (int offset = 0; offset < kEntrySize; offset++) {
      Object val = GetDataEntry(entry, offset);
      CHECK(IsTheHole(val, isolate));
    }
  }
}

void SmallOrderedNameDictionary::SmallOrderedNameDictionaryVerify(
    Isolate* isolate) {
  CHECK(IsSmallOrderedNameDictionary(*this));
  SmallOrderedHashTable<
      SmallOrderedNameDictionary>::SmallOrderedHashTableVerify(isolate);
  for (int entry = NumberOfElements(); entry < NumberOfDeletedElements();
       entry++) {
    for (int offset = 0; offset < kEntrySize; offset++) {
      Object val = GetDataEntry(entry, offset);
      CHECK(IsTheHole(val, isolate) ||
            (PropertyDetails::Empty().AsSmi() == Smi::cast(val)));
    }
  }
}

void SwissNameDictionary::SwissNameDictionaryVerify(Isolate* isolate) {
  this->SwissNameDictionaryVerify(isolate, false);
}

void SwissNameDictionary::SwissNameDictionaryVerify(Isolate* isolate,
                                                    bool slow_checks) {
  DisallowHeapAllocation no_gc;

  CHECK(IsValidCapacity(Capacity()));

  meta_table()->ByteArrayVerify(isolate);

  int seen_deleted = 0;
  int seen_present = 0;

  for (int i = 0; i < Capacity(); i++) {
    ctrl_t ctrl = GetCtrl(i);

    if (IsFull(ctrl) || slow_checks) {
      Object key = KeyAt(i);
      Object value = ValueAtRaw(i);

      if (IsFull(ctrl)) {
        ++seen_present;

        Name name = Name::cast(key);
        if (slow_checks) {
          CHECK_EQ(swiss_table::H2(name->hash()), ctrl);
        }

        CHECK(!IsTheHole(key));
        CHECK(!IsTheHole(value));
        name->NameVerify(isolate);
        Object::ObjectVerify(value, isolate);
      } else if (IsDeleted(ctrl)) {
        ++seen_deleted;
        CHECK(IsTheHole(key));
        CHECK(IsTheHole(value));
      } else if (IsEmpty(ctrl)) {
        CHECK(IsTheHole(key));
        CHECK(IsTheHole(value));
      } else {
        // Something unexpected. Note that we don't use kSentinel at the moment.
        UNREACHABLE();
      }
    }
  }

  CHECK_EQ(seen_present, NumberOfElements());
  if (slow_checks) {
    CHECK_EQ(seen_deleted, NumberOfDeletedElements());

    // Verify copy of first group at end (= after Capacity() slots) of control
    // table.
    for (int i = 0; i < std::min(static_cast<int>(Group::kWidth), Capacity());
         ++i) {
      CHECK_EQ(CtrlTable()[i], CtrlTable()[Capacity() + i]);
    }
    // If 2 * capacity is smaller than the capacity plus group width, the slots
    // after that must be empty.
    for (int i = 2 * Capacity(); i < Capacity() + kGroupWidth; ++i) {
      CHECK_EQ(Ctrl::kEmpty, CtrlTable()[i]);
    }

    for (int enum_index = 0; enum_index < UsedCapacity(); ++enum_index) {
      int entry = EntryForEnumerationIndex(enum_index);
      CHECK_LT(entry, Capacity());
      ctrl_t ctrl = GetCtrl(entry);

      // Enum table must not point to empty slots.
      CHECK(IsFull(ctrl) || IsDeleted(ctrl));
    }
  }
}

void JSRegExp::JSRegExpVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSRegExpVerify(*this, isolate);
  switch (type_tag()) {
    case JSRegExp::ATOM: {
      FixedArray arr = FixedArray::cast(data());
      CHECK(IsString(arr->get(JSRegExp::kAtomPatternIndex)));
      break;
    }
    case JSRegExp::EXPERIMENTAL: {
      FixedArray arr = FixedArray::cast(data());
      Smi uninitialized = Smi::FromInt(JSRegExp::kUninitializedValue);

      Object latin1_code = arr->get(JSRegExp::kIrregexpLatin1CodeIndex);
      Object uc16_code = arr->get(JSRegExp::kIrregexpUC16CodeIndex);
      Object latin1_bytecode = arr->get(JSRegExp::kIrregexpLatin1BytecodeIndex);
      Object uc16_bytecode = arr->get(JSRegExp::kIrregexpUC16BytecodeIndex);

      bool is_compiled = IsCode(latin1_code);
      if (is_compiled) {
        CHECK_EQ(Code::cast(latin1_code)->builtin_id(),
                 Builtin::kRegExpExperimentalTrampoline);
        CHECK_EQ(uc16_code, latin1_code);

        CHECK(IsByteArray(latin1_bytecode));
        CHECK_EQ(uc16_bytecode, latin1_bytecode);
      } else {
        CHECK_EQ(latin1_code, uninitialized);
        CHECK_EQ(uc16_code, uninitialized);

        CHECK_EQ(latin1_bytecode, uninitialized);
        CHECK_EQ(uc16_bytecode, uninitialized);
      }

      CHECK_EQ(arr->get(JSRegExp::kIrregexpMaxRegisterCountIndex),
               uninitialized);
      CHECK(IsSmi(arr->get(JSRegExp::kIrregexpCaptureCountIndex)));
      CHECK_GE(Smi::ToInt(arr->get(JSRegExp::kIrregexpCaptureCountIndex)), 0);
      CHECK_EQ(arr->get(JSRegExp::kIrregexpTicksUntilTierUpIndex),
               uninitialized);
      CHECK_EQ(arr->get(JSRegExp::kIrregexpBacktrackLimit), uninitialized);
      break;
    }
    case JSRegExp::IRREGEXP: {
      bool can_be_interpreted = RegExp::CanGenerateBytecode();

      FixedArray arr = FixedArray::cast(data());
      Object one_byte_data = arr->get(JSRegExp::kIrregexpLatin1CodeIndex);
      // Smi : Not compiled yet (-1).
      // InstructionStream: Compiled irregexp code or trampoline to the
      // interpreter.
      CHECK((IsSmi(one_byte_data) &&
             Smi::ToInt(one_byte_data) == JSRegExp::kUninitializedValue) ||
            IsCode(one_byte_data));
      Object uc16_data = arr->get(JSRegExp::kIrregexpUC16CodeIndex);
      CHECK((IsSmi(uc16_data) &&
             Smi::ToInt(uc16_data) == JSRegExp::kUninitializedValue) ||
            IsCode(uc16_data));

      Object one_byte_bytecode =
          arr->get(JSRegExp::kIrregexpLatin1BytecodeIndex);
      // Smi : Not compiled yet (-1).
      // ByteArray: Bytecode to interpret regexp.
      CHECK((IsSmi(one_byte_bytecode) &&
             Smi::ToInt(one_byte_bytecode) == JSRegExp::kUninitializedValue) ||
            (can_be_interpreted && IsByteArray(one_byte_bytecode)));
      Object uc16_bytecode = arr->get(JSRegExp::kIrregexpUC16BytecodeIndex);
      CHECK((IsSmi(uc16_bytecode) &&
             Smi::ToInt(uc16_bytecode) == JSRegExp::kUninitializedValue) ||
            (can_be_interpreted && IsByteArray(uc16_bytecode)));

      CHECK_IMPLIES(IsSmi(one_byte_data), IsSmi(one_byte_bytecode));
      CHECK_IMPLIES(IsSmi(uc16_data), IsSmi(uc16_bytecode));

      CHECK(IsSmi(arr->get(JSRegExp::kIrregexpCaptureCountIndex)));
      CHECK_GE(Smi::ToInt(arr->get(JSRegExp::kIrregexpCaptureCountIndex)), 0);
      CHECK(IsSmi(arr->get(JSRegExp::kIrregexpMaxRegisterCountIndex)));
      CHECK(IsSmi(arr->get(JSRegExp::kIrregexpTicksUntilTierUpIndex)));
      CHECK(IsSmi(arr->get(JSRegExp::kIrregexpBacktrackLimit)));
      break;
    }
    default:
      CHECK_EQ(JSRegExp::NOT_COMPILED, type_tag());
      CHECK(IsUndefined(data(), isolate));
      break;
  }
}

void JSProxy::JSProxyVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSProxyVerify(*this, isolate);
  CHECK(IsJSFunction(map()->GetConstructor()));
  if (!IsRevoked()) {
    CHECK_EQ(IsCallable(target()), map()->is_callable());
    CHECK_EQ(IsConstructor(target()), map()->is_constructor());
  }
  CHECK(IsNull(map()->prototype(), isolate));
  // There should be no properties on a Proxy.
  CHECK_EQ(0, map()->NumberOfOwnDescriptors());
}

void JSArrayBuffer::JSArrayBufferVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSArrayBufferVerify(*this, isolate);
  if (FIELD_SIZE(kOptionalPaddingOffset) != 0) {
    CHECK_EQ(4, FIELD_SIZE(kOptionalPaddingOffset));
    CHECK_EQ(0,
             *reinterpret_cast<uint32_t*>(address() + kOptionalPaddingOffset));
  }
}

void JSArrayBufferView::JSArrayBufferViewVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSArrayBufferViewVerify(*this, isolate);
  CHECK_LE(byte_length(), JSArrayBuffer::kMaxByteLength);
  CHECK_LE(byte_offset(), JSArrayBuffer::kMaxByteLength);
}

void JSTypedArray::JSTypedArrayVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSTypedArrayVerify(*this, isolate);
  CHECK_LE(GetLength(), JSTypedArray::kMaxLength);
}

void JSDataView::JSDataViewVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSDataViewVerify(*this, isolate);
  CHECK(!IsVariableLength());
  if (!WasDetached()) {
    CHECK_EQ(reinterpret_cast<uint8_t*>(
                 JSArrayBuffer::cast(buffer())->backing_store()) +
                 byte_offset(),
             data_pointer());
  }
}

void JSRabGsabDataView::JSRabGsabDataViewVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::JSRabGsabDataViewVerify(*this, isolate);
  CHECK(IsVariableLength());
  if (!WasDetached()) {
    CHECK_EQ(reinterpret_cast<uint8_t*>(
                 JSArrayBuffer::cast(buffer())->backing_store()) +
                 byte_offset(),
             data_pointer());
  }
}

void AsyncGeneratorRequest::AsyncGeneratorRequestVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::AsyncGeneratorRequestVerify(*this, isolate);
  CHECK_GE(resume_mode(), JSGeneratorObject::kNext);
  CHECK_LE(resume_mode(), JSGeneratorObject::kThrow);
}

void BigIntBase::BigIntBaseVerify(Isolate* isolate) {
  CHECK_GE(length(), 0);
  CHECK_IMPLIES(is_zero(), !sign());  // There is no -0n.
}

void SourceTextModuleInfoEntry::SourceTextModuleInfoEntryVerify(
    Isolate* isolate) {
  TorqueGeneratedClassVerifiers::SourceTextModuleInfoEntryVerify(*this,
                                                                 isolate);
  CHECK_IMPLIES(IsString(import_name()), module_request() >= 0);
  CHECK_IMPLIES(IsString(export_name()) && IsString(import_name()),
                IsUndefined(local_name(), isolate));
}

void Module::ModuleVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ModuleVerify(*this, isolate);

  CHECK_EQ(status() == Module::kErrored, !IsTheHole(exception(), isolate));

  CHECK(IsUndefined(module_namespace(), isolate) ||
        IsJSModuleNamespace(module_namespace()));
  if (IsJSModuleNamespace(module_namespace())) {
    CHECK_LE(Module::kLinking, status());
    CHECK_EQ(JSModuleNamespace::cast(module_namespace())->module(), *this);
  }

  if (!(status() == kErrored || status() == kEvaluating ||
        status() == kEvaluated)) {
    CHECK(IsUndefined(top_level_capability()));
  }

  CHECK_NE(hash(), 0);
}

void ModuleRequest::ModuleRequestVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ModuleRequestVerify(*this, isolate);
  CHECK_EQ(0,
           import_assertions()->length() % ModuleRequest::kAssertionEntrySize);

  for (int i = 0; i < import_assertions()->length();
       i += ModuleRequest::kAssertionEntrySize) {
    CHECK(IsString(import_assertions()->get(i)));      // Assertion key
    CHECK(IsString(import_assertions()->get(i + 1)));  // Assertion value
    CHECK(IsSmi(import_assertions()->get(i + 2)));     // Assertion location
  }
}

void SourceTextModule::SourceTextModuleVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::SourceTextModuleVerify(*this, isolate);

  if (status() == kErrored) {
    CHECK(IsSharedFunctionInfo(code()));
  } else if (status() == kEvaluating || status() == kEvaluated) {
    CHECK(IsJSGeneratorObject(code()));
  } else {
    if (status() == kLinked) {
      CHECK(IsJSGeneratorObject(code()));
    } else if (status() == kLinking) {
      CHECK(IsJSFunction(code()));
    } else if (status() == kPreLinking) {
      CHECK(IsSharedFunctionInfo(code()));
    } else if (status() == kUnlinked) {
      CHECK(IsSharedFunctionInfo(code()));
    }
    CHECK(!AsyncParentModuleCount());
    CHECK(!pending_async_dependencies());
    CHECK(!IsAsyncEvaluating());
  }

  CHECK_EQ(requested_modules()->length(), info()->module_requests()->length());
}

void SyntheticModule::SyntheticModuleVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::SyntheticModuleVerify(*this, isolate);

  for (int i = 0; i < export_names()->length(); i++) {
    CHECK(IsString(export_names()->get(i)));
  }
}

void PrototypeInfo::PrototypeInfoVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::PrototypeInfoVerify(*this, isolate);
  if (IsWeakArrayList(prototype_users())) {
    PrototypeUsers::Verify(WeakArrayList::cast(prototype_users()));
  } else {
    CHECK(IsSmi(prototype_users()));
  }
}

void PrototypeUsers::Verify(WeakArrayList array) {
  if (array->length() == 0) {
    // Allow empty & uninitialized lists.
    return;
  }
  // Verify empty slot chain.
  int empty_slot = Smi::ToInt(empty_slot_index(array));
  int empty_slots_count = 0;
  while (empty_slot != kNoEmptySlotsMarker) {
    CHECK_GT(empty_slot, 0);
    CHECK_LT(empty_slot, array->length());
    empty_slot = array->Get(empty_slot).ToSmi().value();
    ++empty_slots_count;
  }

  // Verify that all elements are either weak pointers or SMIs marking empty
  // slots.
  int weak_maps_count = 0;
  for (int i = kFirstIndex; i < array->length(); ++i) {
    HeapObject heap_object;
    MaybeObject object = array->Get(i);
    if ((object->GetHeapObjectIfWeak(&heap_object) && IsMap(heap_object)) ||
        object->IsCleared()) {
      ++weak_maps_count;
    } else {
      CHECK(IsSmi(object));
    }
  }

  CHECK_EQ(weak_maps_count + empty_slots_count + 1, array->length());
}

void EnumCache::EnumCacheVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::EnumCacheVerify(*this, isolate);
  Heap* heap = isolate->heap();
  if (*this == ReadOnlyRoots(heap).empty_enum_cache()) {
    CHECK_EQ(ReadOnlyRoots(heap).empty_fixed_array(), keys());
    CHECK_EQ(ReadOnlyRoots(heap).empty_fixed_array(), indices());
  }
}

void ObjectBoilerplateDescription::ObjectBoilerplateDescriptionVerify(
    Isolate* isolate) {
  CHECK(IsObjectBoilerplateDescription(*this));
  CHECK_GE(this->length(),
           ObjectBoilerplateDescription::kDescriptionStartIndex);
  this->FixedArrayVerify(isolate);
  for (int i = 0; i < length(); ++i) {
    // No ThinStrings in the boilerplate.
    CHECK(!IsThinString(get(isolate, i), isolate));
  }
}

#if V8_ENABLE_WEBASSEMBLY

void WasmInstanceObject::WasmInstanceObjectVerify(Isolate* isolate) {
  JSObjectVerify(isolate);
  CHECK(IsWasmInstanceObject(*this));

  // Just generically check all tagged fields. Don't check the untagged fields,
  // as some of them might still contain the "undefined" value if the
  // WasmInstanceObject is not fully set up yet.
  for (int offset = kHeaderSize; offset < kEndOfStrongFieldsOffset;
       offset += kTaggedSize) {
    VerifyObjectField(isolate, offset);
  }
}

void WasmValueObject::WasmValueObjectVerify(Isolate* isolate) {
  JSObjectVerify(isolate);
  CHECK(IsWasmValueObject(*this));
}

void WasmExceptionPackage::WasmExceptionPackageVerify(Isolate* isolate) {
  JSObjectVerify(isolate);
  CHECK(IsWasmExceptionPackage(*this));
}

void WasmExportedFunctionData::WasmExportedFunctionDataVerify(
    Isolate* isolate) {
  TorqueGeneratedClassVerifiers::WasmExportedFunctionDataVerify(*this, isolate);
  CHECK(
      wrapper_code()->kind() == CodeKind::JS_TO_WASM_FUNCTION ||
      wrapper_code()->kind() == CodeKind::C_WASM_ENTRY ||
      (wrapper_code()->is_builtin() &&
       (wrapper_code()->builtin_id() == Builtin::kJSToWasmWrapper ||
        wrapper_code()->builtin_id() == Builtin::kWasmReturnPromiseOnSuspend)));
}

#endif  // V8_ENABLE_WEBASSEMBLY

void DataHandler::DataHandlerVerify(Isolate* isolate) {
  // Don't call TorqueGeneratedClassVerifiers::DataHandlerVerify because the
  // Torque definition of this class includes all of the optional fields.

  // This assertion exists to encourage updating this verification function if
  // new fields are added in the Torque class layout definition.
  static_assert(DataHandler::kHeaderSize == 6 * kTaggedSize);

  StructVerify(isolate);
  CHECK(IsDataHandler(*this));
  VerifyPointer(isolate, smi_handler(isolate));
  CHECK_IMPLIES(!IsSmi(smi_handler()),
                IsStoreHandler(*this) && IsCode(smi_handler()));
  VerifyPointer(isolate, validity_cell(isolate));
  CHECK(IsSmi(validity_cell()) || IsCell(validity_cell()));
  int data_count = data_field_count();
  if (data_count >= 1) {
    VerifyMaybeObjectField(isolate, kData1Offset);
  }
  if (data_count >= 2) {
    VerifyMaybeObjectField(isolate, kData2Offset);
  }
  if (data_count >= 3) {
    VerifyMaybeObjectField(isolate, kData3Offset);
  }
}

void LoadHandler::LoadHandlerVerify(Isolate* isolate) {
  DataHandler::DataHandlerVerify(isolate);
  // TODO(ishell): check handler integrity
}

void StoreHandler::StoreHandlerVerify(Isolate* isolate) {
  DataHandler::DataHandlerVerify(isolate);
  // TODO(ishell): check handler integrity
}

void CallHandlerInfo::CallHandlerInfoVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::CallHandlerInfoVerify(*this, isolate);
  CHECK(map() == ReadOnlyRoots(isolate).side_effect_call_handler_info_map() ||
        map() ==
            ReadOnlyRoots(isolate).side_effect_free_call_handler_info_map());
}

void AllocationSite::AllocationSiteVerify(Isolate* isolate) {
  CHECK(IsAllocationSite(*this));
  CHECK(IsDependentCode(dependent_code()));
  CHECK(IsSmi(transition_info_or_boilerplate()) ||
        IsJSObject(transition_info_or_boilerplate()));
  CHECK(IsAllocationSite(nested_site()) || nested_site() == Smi::zero());
}

void Script::ScriptVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ScriptVerify(*this, isolate);
#if V8_ENABLE_WEBASSEMBLY
  if (type() == Script::Type::kWasm) {
    CHECK_EQ(line_ends(), ReadOnlyRoots(isolate).empty_fixed_array());
  } else {
    CHECK(CanHaveLineEnds());
  }
#else   // V8_ENABLE_WEBASSEMBLY
  CHECK(CanHaveLineEnds());
#endif  // V8_ENABLE_WEBASSEMBLY
  for (int i = 0; i < shared_function_info_count(); ++i) {
    MaybeObject maybe_object = shared_function_infos()->Get(i);
    HeapObject heap_object;
    CHECK(maybe_object->IsWeak() || maybe_object->IsCleared() ||
          (maybe_object->GetHeapObjectIfStrong(&heap_object) &&
           IsUndefined(heap_object, isolate)));
  }
}

void NormalizedMapCache::NormalizedMapCacheVerify(Isolate* isolate) {
  WeakFixedArray::cast(*this)->WeakFixedArrayVerify(isolate);
  if (v8_flags.enable_slow_asserts) {
    for (int i = 0; i < length(); i++) {
      MaybeObject e = WeakFixedArray::Get(i);
      HeapObject heap_object;
      if (e->GetHeapObjectIfWeak(&heap_object)) {
        Map::cast(heap_object)->DictionaryMapVerify(isolate);
      } else {
        CHECK(e->IsCleared() || (e->GetHeapObjectIfStrong(&heap_object) &&
                                 IsUndefined(heap_object, isolate)));
      }
    }
  }
}

void PreparseData::PreparseDataVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::PreparseDataVerify(*this, isolate);
  CHECK_LE(0, data_length());
  CHECK_LE(0, children_length());

  for (int i = 0; i < children_length(); ++i) {
    Object child = get_child_raw(i);
    CHECK(IsNull(child) || IsPreparseData(child));
    VerifyPointer(isolate, child);
  }
}

void CallSiteInfo::CallSiteInfoVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::CallSiteInfoVerify(*this, isolate);
#if V8_ENABLE_WEBASSEMBLY
  CHECK_IMPLIES(IsAsmJsWasm(), IsWasm());
  CHECK_IMPLIES(IsWasm(), IsWasmInstanceObject(receiver_or_instance()));
  CHECK_IMPLIES(IsWasm() || IsBuiltin(), IsSmi(function()));
  CHECK_IMPLIES(!IsWasm() && !IsBuiltin(), IsJSFunction(function()));
  CHECK_IMPLIES(IsAsync(), !IsWasm());
  CHECK_IMPLIES(IsConstructor(), !IsWasm());
#endif  // V8_ENABLE_WEBASSEMBLY
}

void FunctionTemplateRareData::FunctionTemplateRareDataVerify(
    Isolate* isolate) {
  CHECK(IsFixedArray(c_function_overloads()) ||
        IsUndefined(c_function_overloads(), isolate));
}

void StackFrameInfo::StackFrameInfoVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::StackFrameInfoVerify(*this, isolate);
}

void ErrorStackData::ErrorStackDataVerify(Isolate* isolate) {
  TorqueGeneratedClassVerifiers::ErrorStackDataVerify(*this, isolate);
  CHECK_IMPLIES(!IsFixedArray(call_site_infos_or_formatted_stack()),
                IsFixedArray(limit_or_stack_frame_infos()));
}

// Helper class for verifying the string table.
class StringTableVerifier : public RootVisitor {
 public:
  explicit StringTableVerifier(Isolate* isolate) : isolate_(isolate) {}

  void VisitRootPointers(Root root, const char* description,
                         FullObjectSlot start, FullObjectSlot end) override {
    UNREACHABLE();
  }
  void VisitRootPointers(Root root, const char* description,
                         OffHeapObjectSlot start,
                         OffHeapObjectSlot end) override {
    // Visit all HeapObject pointers in [start, end).
    for (OffHeapObjectSlot p = start; p < end; ++p) {
      Object o = p.load(isolate_);
      DCHECK(!HasWeakHeapObjectTag(o));
      if (IsHeapObject(o)) {
        HeapObject object = HeapObject::cast(o);
        // Check that the string is actually internalized.
        CHECK(IsInternalizedString(object));
      }
    }
  }

 private:
  Isolate* isolate_;
};

void StringTable::VerifyIfOwnedBy(Isolate* isolate) {
  DCHECK_EQ(isolate->string_table(), this);
  if (!isolate->OwnsStringTables()) return;
  StringTableVerifier verifier(isolate);
  IterateElements(&verifier);
}

#endif  // VERIFY_HEAP

#ifdef DEBUG

void JSObject::IncrementSpillStatistics(Isolate* isolate,
                                        SpillInformation* info) {
  info->number_of_objects_++;
  // Named properties
  if (HasFastProperties()) {
    info->number_of_objects_with_fast_properties_++;
    info->number_of_fast_used_fields_ += map()->NextFreePropertyIndex();
    info->number_of_fast_unused_fields_ += map()->UnusedPropertyFields();
  } else if (IsJSGlobalObject(*this)) {
    GlobalDictionary dict =
        JSGlobalObject::cast(*this)->global_dictionary(kAcquireLoad);
    info->number_of_slow_used_properties_ += dict->NumberOfElements();
    info->number_of_slow_unused_properties_ +=
        dict->Capacity() - dict->NumberOfElements();
  } else if (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
    SwissNameDictionary dict = property_dictionary_swiss();
    info->number_of_slow_used_properties_ += dict->NumberOfElements();
    info->number_of_slow_unused_properties_ +=
        dict->Capacity() - dict->NumberOfElements();
  } else {
    NameDictionary dict = property_dictionary();
    info->number_of_slow_used_properties_ += dict->NumberOfElements();
    info->number_of_slow_unused_properties_ +=
        dict->Capacity() - dict->NumberOfElements();
  }
  // Indexed properties
  switch (GetElementsKind()) {
    case HOLEY_SMI_ELEMENTS:
    case PACKED_SMI_ELEMENTS:
    case HOLEY_DOUBLE_ELEMENTS:
    case PACKED_DOUBLE_ELEMENTS:
    case HOLEY_ELEMENTS:
    case HOLEY_FROZEN_ELEMENTS:
    case HOLEY_SEALED_ELEMENTS:
    case HOLEY_NONEXTENSIBLE_ELEMENTS:
    case PACKED_ELEMENTS:
    case PACKED_FROZEN_ELEMENTS:
    case PACKED_SEALED_ELEMENTS:
    case PACKED_NONEXTENSIBLE_ELEMENTS:
    case FAST_STRING_WRAPPER_ELEMENTS:
    case SHARED_ARRAY_ELEMENTS: {
      info->number_of_objects_with_fast_elements_++;
      int holes = 0;
      FixedArray e = FixedArray::cast(elements());
      int len = e->length();
      for (int i = 0; i < len; i++) {
        if (IsTheHole(e->get(i), isolate)) holes++;
      }
      info->number_of_fast_used_elements_ += len - holes;
      info->number_of_fast_unused_elements_ += holes;
      break;
    }

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) case TYPE##_ELEMENTS:

      TYPED_ARRAYS(TYPED_ARRAY_CASE)
      RAB_GSAB_TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
      {
        info->number_of_objects_with_fast_elements_++;
        FixedArrayBase e = FixedArrayBase::cast(elements());
        info->number_of_fast_used_elements_ += e->length();
        break;
      }
    case DICTIONARY_ELEMENTS:
    case SLOW_STRING_WRAPPER_ELEMENTS: {
      NumberDictionary dict = element_dictionary();
      info->number_of_slow_used_elements_ += dict->NumberOfElements();
      info->number_of_slow_unused_elements_ +=
          dict->Capacity() - dict->NumberOfElements();
      break;
    }
    case FAST_SLOPPY_ARGUMENTS_ELEMENTS:
    case SLOW_SLOPPY_ARGUMENTS_ELEMENTS:
    case WASM_ARRAY_ELEMENTS:
    case NO_ELEMENTS:
      break;
  }
}

void JSObject::SpillInformation::Clear() {
  number_of_objects_ = 0;
  number_of_objects_with_fast_properties_ = 0;
  number_of_objects_with_fast_elements_ = 0;
  number_of_fast_used_fields_ = 0;
  number_of_fast_unused_fields_ = 0;
  number_of_slow_used_properties_ = 0;
  number_of_slow_unused_properties_ = 0;
  number_of_fast_used_elements_ = 0;
  number_of_fast_unused_elements_ = 0;
  number_of_slow_used_elements_ = 0;
  number_of_slow_unused_elements_ = 0;
}

void JSObject::SpillInformation::Print() {
  PrintF("\n  JSObject Spill Statistics (#%d):\n", number_of_objects_);

  PrintF("    - fast properties (#%d): %d (used) %d (unused)\n",
         number_of_objects_with_fast_properties_, number_of_fast_used_fields_,
         number_of_fast_unused_fields_);

  PrintF("    - slow properties (#%d): %d (used) %d (unused)\n",
         number_of_objects_ - number_of_objects_with_fast_properties_,
         number_of_slow_used_properties_, number_of_slow_unused_properties_);

  PrintF("    - fast elements (#%d): %d (used) %d (unused)\n",
         number_of_objects_with_fast_elements_, number_of_fast_used_elements_,
         number_of_fast_unused_elements_);

  PrintF("    - slow elements (#%d): %d (used) %d (unused)\n",
         number_of_objects_ - number_of_objects_with_fast_elements_,
         number_of_slow_used_elements_, number_of_slow_unused_elements_);

  PrintF("\n");
}

bool DescriptorArray::IsSortedNoDuplicates() {
  Name current_key;
  uint32_t current = 0;
  for (int i = 0; i < number_of_descriptors(); i++) {
    Name key = GetSortedKey(i);
    uint32_t hash;
    const bool has_hash = key->TryGetHash(&hash);
    CHECK(has_hash);
    if (key == current_key) {
      Print(*this);
      return false;
    }
    current_key = key;
    if (hash < current) {
      Print(*this);
      return false;
    }
    current = hash;
  }
  return true;
}

bool TransitionArray::IsSortedNoDuplicates() {
  Name prev_key;
  PropertyKind prev_kind = PropertyKind::kData;
  PropertyAttributes prev_attributes = NONE;
  uint32_t prev_hash = 0;

  for (int i = 0; i < number_of_transitions(); i++) {
    Name key = GetSortedKey(i);
    uint32_t hash;
    const bool has_hash = key->TryGetHash(&hash);
    CHECK(has_hash);
    PropertyKind kind = PropertyKind::kData;
    PropertyAttributes attributes = NONE;
    if (!TransitionsAccessor::IsSpecialTransition(key->GetReadOnlyRoots(),
                                                  key)) {
      Map target = GetTarget(i);
      PropertyDetails details =
          TransitionsAccessor::GetTargetDetails(key, target);
      kind = details.kind();
      attributes = details.attributes();
    } else {
      // Duplicate entries are not allowed for non-property transitions.
      DCHECK_NE(prev_key, key);
    }

    int cmp = CompareKeys(prev_key, prev_hash, prev_kind, prev_attributes, key,
                          hash, kind, attributes);
    if (cmp >= 0) {
      Print(*this);
      return false;
    }
    prev_key = key;
    prev_hash = hash;
    prev_attributes = attributes;
    prev_kind = kind;
  }
  return true;
}

bool TransitionsAccessor::IsSortedNoDuplicates() {
  // Simple and non-existent transitions are always sorted.
  if (encoding() != kFullTransitionArray) return true;
  return transitions()->IsSortedNoDuplicates();
}

static bool CheckOneBackPointer(Map current_map, Object target) {
  return !IsMap(target) || Map::cast(target)->GetBackPointer() == current_map;
}

bool TransitionsAccessor::IsConsistentWithBackPointers() {
  int num_transitions = NumberOfTransitions();
  for (int i = 0; i < num_transitions; i++) {
    Map target = GetTarget(i);
    if (!CheckOneBackPointer(map_, target)) return false;
  }
  return true;
}

#undef USE_TORQUE_VERIFIER

#endif  // DEBUG

}  // namespace internal
}  // namespace v8
