// Copyright (c) 2009-2021, Google LLC
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Google LLC nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <cstdint>
#include <memory>

#include "google/protobuf/descriptor.pb.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/compiler/plugin.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/wire_format.h"
#include "upb/mini_table/encode_internal.hpp"
#include "upb/mini_table/enum_internal.h"
#include "upb/mini_table/extension_internal.h"
#include "upbc/common.h"
#include "upbc/file_layout.h"
#include "upbc/names.h"

// Must be last.
#include "upb/port/def.inc"

namespace upbc {
namespace {

namespace protoc = ::google::protobuf::compiler;
namespace protobuf = ::google::protobuf;

// Returns fields in order of "hotness", eg. how frequently they appear in
// serialized payloads. Ideally this will use a profile. When we don't have
// that, we assume that fields with smaller numbers are used more frequently.
inline std::vector<const google::protobuf::FieldDescriptor*> FieldHotnessOrder(
    const google::protobuf::Descriptor* message) {
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  size_t field_count = message->field_count();
  fields.reserve(field_count);
  for (size_t i = 0; i < field_count; i++) {
    fields.push_back(message->field(i));
  }
  std::sort(
      fields.begin(), fields.end(),
      [](const google::protobuf::FieldDescriptor* a, const google::protobuf::FieldDescriptor* b) {
        return std::make_pair(!a->is_required(), a->number()) <
               std::make_pair(!b->is_required(), b->number());
      });
  return fields;
}

std::string SourceFilename(const google::protobuf::FileDescriptor* file) {
  return StripExtension(file->name()) + ".upb.c";
}

std::string MessageInit(const protobuf::Descriptor* descriptor) {
  return MessageName(descriptor) + "_msg_init";
}

std::string EnumInit(const protobuf::EnumDescriptor* descriptor) {
  return ToCIdent(descriptor->full_name()) + "_enum_init";
}

std::string ExtensionIdentBase(const protobuf::FieldDescriptor* ext) {
  assert(ext->is_extension());
  std::string ext_scope;
  if (ext->extension_scope()) {
    return MessageName(ext->extension_scope());
  } else {
    return ToCIdent(ext->file()->package());
  }
}

std::string ExtensionLayout(const google::protobuf::FieldDescriptor* ext) {
  return absl::StrCat(ExtensionIdentBase(ext), "_", ext->name(), "_ext");
}

const char* kEnumsInit = "enums_layout";
const char* kExtensionsInit = "extensions_layout";
const char* kMessagesInit = "messages_layout";

std::string EnumValueSymbol(const protobuf::EnumValueDescriptor* value) {
  return ToCIdent(value->full_name());
}

std::string CTypeInternal(const protobuf::FieldDescriptor* field,
                          bool is_const) {
  std::string maybe_const = is_const ? "const " : "";
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      std::string maybe_struct =
          field->file() != field->message_type()->file() ? "struct " : "";
      return maybe_const + maybe_struct + MessageName(field->message_type()) +
             "*";
    }
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return "bool";
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return "float";
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return "int32_t";
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return "uint32_t";
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return "double";
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
      return "int64_t";
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return "uint64_t";
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return "upb_StringView";
    default:
      fprintf(stderr, "Unexpected type");
      abort();
  }
}

std::string FloatToCLiteral(float value) {
  if (value == std::numeric_limits<float>::infinity()) {
    return "kUpb_FltInfinity";
  } else if (value == -std::numeric_limits<float>::infinity()) {
    return "-kUpb_FltInfinity";
  } else if (std::isnan(value)) {
    return "kUpb_NaN";
  } else {
    return absl::StrCat(value);
  }
}

std::string DoubleToCLiteral(double value) {
  if (value == std::numeric_limits<double>::infinity()) {
    return "kUpb_Infinity";
  } else if (value == -std::numeric_limits<double>::infinity()) {
    return "-kUpb_Infinity";
  } else if (std::isnan(value)) {
    return "kUpb_NaN";
  } else {
    return absl::StrCat(value);
  }
}

std::string FieldDefault(const protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return "NULL";
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return absl::Substitute("upb_StringView_FromString(\"$0\")",
                              absl::CEscape(field->default_value_string()));
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
      return absl::Substitute("(int32_t)$0", field->default_value_int32());
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
      return absl::Substitute("(int64_t)$0ll", field->default_value_int64());
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return absl::Substitute("(uint32_t)$0u", field->default_value_uint32());
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return absl::Substitute("(uint64_t)$0ull", field->default_value_uint64());
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return FloatToCLiteral(field->default_value_float());
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return DoubleToCLiteral(field->default_value_double());
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return field->default_value_bool() ? "true" : "false";
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      // Use a number instead of a symbolic name so that we don't require
      // this enum's header to be included.
      return absl::StrCat(field->default_value_enum()->number());
  }
  ABSL_ASSERT(false);
  return "XXX";
}

std::string CType(const protobuf::FieldDescriptor* field) {
  return CTypeInternal(field, false);
}

std::string CTypeConst(const protobuf::FieldDescriptor* field) {
  return CTypeInternal(field, true);
}

std::string MapKeyCType(const protobuf::FieldDescriptor* map_field) {
  return CType(map_field->message_type()->map_key());
}

std::string MapValueCType(const protobuf::FieldDescriptor* map_field) {
  return CType(map_field->message_type()->map_value());
}

std::string MapKeySize(const protobuf::FieldDescriptor* map_field,
                       absl::string_view expr) {
  return map_field->message_type()->map_key()->cpp_type() ==
                 protobuf::FieldDescriptor::CPPTYPE_STRING
             ? "0"
             : absl::StrCat("sizeof(", expr, ")");
}

std::string MapValueSize(const protobuf::FieldDescriptor* map_field,
                         absl::string_view expr) {
  return map_field->message_type()->map_value()->cpp_type() ==
                 protobuf::FieldDescriptor::CPPTYPE_STRING
             ? "0"
             : absl::StrCat("sizeof(", expr, ")");
}

std::string FieldInitializer(const FileLayout& layout,
                             const protobuf::FieldDescriptor* field);

void DumpEnumValues(const protobuf::EnumDescriptor* desc, Output& output) {
  std::vector<const protobuf::EnumValueDescriptor*> values;
  for (int i = 0; i < desc->value_count(); i++) {
    values.push_back(desc->value(i));
  }
  std::sort(values.begin(), values.end(),
            [](const protobuf::EnumValueDescriptor* a,
               const protobuf::EnumValueDescriptor* b) {
              return a->number() < b->number();
            });

  for (size_t i = 0; i < values.size(); i++) {
    auto value = values[i];
    output("  $0 = $1", EnumValueSymbol(value), value->number());
    if (i != values.size() - 1) {
      output(",");
    }
    output("\n");
  }
}

std::string GetFieldRep(const FileLayout& layout,
                        const protobuf::FieldDescriptor* field);

void GenerateExtensionInHeader(const protobuf::FieldDescriptor* ext,
                               const FileLayout& layout, Output& output) {
  output(
      R"cc(
        UPB_INLINE bool $0_has_$1(const struct $2* msg) {
          return _upb_Message_HasExtensionField(msg, &$3);
        }
      )cc",
      ExtensionIdentBase(ext), ext->name(), MessageName(ext->containing_type()),
      ExtensionLayout(ext));

  output(
      R"cc(
        UPB_INLINE void $0_clear_$1(struct $2* msg) {
          _upb_Message_ClearExtensionField(msg, &$3);
        }
      )cc",
      ExtensionIdentBase(ext), ext->name(), MessageName(ext->containing_type()),
      ExtensionLayout(ext));

  if (ext->is_repeated()) {
    // TODO(b/259861668): We need generated accessors for repeated extensions.
  } else {
    output(
        R"cc(
          UPB_INLINE $0 $1_$2(const struct $3* msg) {
            const upb_MiniTableExtension* ext = &$4;
            UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
            UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == $5);
            $0 default_val = $6;
            $0 ret;
            _upb_Message_GetExtensionField(msg, ext, &default_val, &ret);
            return ret;
          }
        )cc",
        CTypeConst(ext), ExtensionIdentBase(ext), ext->name(),
        MessageName(ext->containing_type()), ExtensionLayout(ext),
        GetFieldRep(layout, ext), FieldDefault(ext));
    output(
        R"cc(
          UPB_INLINE void $1_set_$2(struct $3* msg, $0 val, upb_Arena* arena) {
            const upb_MiniTableExtension* ext = &$4;
            UPB_ASSUME(!upb_IsRepeatedOrMap(&ext->field));
            UPB_ASSUME(_upb_MiniTableField_GetRep(&ext->field) == $5);
            bool ok = _upb_Message_SetExtensionField(msg, ext, &val, arena);
            UPB_ASSERT(ok);
          }
        )cc",
        CTypeConst(ext), ExtensionIdentBase(ext), ext->name(),
        MessageName(ext->containing_type()), ExtensionLayout(ext),
        GetFieldRep(layout, ext));
  }
}

void GenerateMessageFunctionsInHeader(const protobuf::Descriptor* message,
                                      Output& output) {
  // TODO(b/235839510): The generated code here does not check the return values
  // from upb_Encode(). How can we even fix this without breaking other things?
  output(
      R"cc(
        UPB_INLINE $0* $0_new(upb_Arena* arena) {
          return ($0*)_upb_Message_New(&$1, arena);
        }
        UPB_INLINE $0* $0_parse(const char* buf, size_t size, upb_Arena* arena) {
          $0* ret = $0_new(arena);
          if (!ret) return NULL;
          if (upb_Decode(buf, size, ret, &$1, NULL, 0, arena) != kUpb_DecodeStatus_Ok) {
            return NULL;
          }
          return ret;
        }
        UPB_INLINE $0* $0_parse_ex(const char* buf, size_t size,
                                   const upb_ExtensionRegistry* extreg,
                                   int options, upb_Arena* arena) {
          $0* ret = $0_new(arena);
          if (!ret) return NULL;
          if (upb_Decode(buf, size, ret, &$1, extreg, options, arena) !=
              kUpb_DecodeStatus_Ok) {
            return NULL;
          }
          return ret;
        }
        UPB_INLINE char* $0_serialize(const $0* msg, upb_Arena* arena, size_t* len) {
          char* ptr;
          (void)upb_Encode(msg, &$1, 0, arena, &ptr, len);
          return ptr;
        }
        UPB_INLINE char* $0_serialize_ex(const $0* msg, int options,
                                         upb_Arena* arena, size_t* len) {
          char* ptr;
          (void)upb_Encode(msg, &$1, options, arena, &ptr, len);
          return ptr;
        }
      )cc",
      MessageName(message), MessageInit(message));
}

void GenerateOneofInHeader(const protobuf::OneofDescriptor* oneof,
                           const FileLayout& layout, absl::string_view msg_name,
                           Output& output) {
  std::string fullname = ToCIdent(oneof->full_name());
  output("typedef enum {\n");
  for (int j = 0; j < oneof->field_count(); j++) {
    const protobuf::FieldDescriptor* field = oneof->field(j);
    output("  $0_$1 = $2,\n", fullname, field->name(), field->number());
  }
  output(
      "  $0_NOT_SET = 0\n"
      "} $0_oneofcases;\n",
      fullname);
  output(
      R"cc(
        UPB_INLINE $0_oneofcases $1_$2_case(const $1* msg) {
          const upb_MiniTableField field = $3;
          return ($0_oneofcases)upb_Message_WhichOneofFieldNumber(msg, &field);
        }
      )cc",
      fullname, msg_name, oneof->name(),
      FieldInitializer(layout, oneof->field(0)));
}

void GenerateHazzer(const protobuf::FieldDescriptor* field,
                    const FileLayout& layout, absl::string_view msg_name,
                    const NameToFieldDescriptorMap& field_names,
                    Output& output) {
  std::string resolved_name = ResolveFieldName(field, field_names);
  if (field->has_presence()) {
    output(
        R"cc(
          UPB_INLINE bool $0_has_$1(const $0* msg) {
            const upb_MiniTableField field = $2;
            return _upb_Message_HasNonExtensionField(msg, &field);
          }
        )cc",
        msg_name, resolved_name, FieldInitializer(layout, field));
  } else if (field->is_map()) {
    // TODO(b/259616267): remove.
    output(
        R"cc(
          UPB_INLINE bool $0_has_$1(const $0* msg) {
            return $0_$1_size(msg) != 0;
          }
        )cc",
        msg_name, resolved_name);
  } else if (field->is_repeated()) {
    // TODO(b/259616267): remove.
    output(
        R"cc(
          UPB_INLINE bool $0_has_$1(const $0* msg) {
            size_t size;
            $0_$1(msg, &size);
            return size != 0;
          }
        )cc",
        msg_name, resolved_name);
  }
}

void GenerateClear(const protobuf::FieldDescriptor* field,
                   const FileLayout& layout, absl::string_view msg_name,
                   const NameToFieldDescriptorMap& field_names,
                   Output& output) {
  if (field == field->containing_type()->map_key() ||
      field == field->containing_type()->map_value()) {
    // Cannot be cleared.
    return;
  }
  std::string resolved_name = ResolveFieldName(field, field_names);
  output(
      R"cc(
        UPB_INLINE void $0_clear_$1($0* msg) {
          const upb_MiniTableField field = $2;
          _upb_Message_ClearNonExtensionField(msg, &field);
        }
      )cc",
      msg_name, resolved_name, FieldInitializer(layout, field));
}

void GenerateMapGetters(const protobuf::FieldDescriptor* field,
                        const FileLayout& layout, absl::string_view msg_name,
                        const NameToFieldDescriptorMap& field_names,
                        Output& output) {
  std::string resolved_name = ResolveFieldName(field, field_names);
  output(
      R"cc(
        UPB_INLINE size_t $0_$1_size(const $0* msg) {
          const upb_MiniTableField field = $2;
          const upb_Map* map = upb_Message_GetMap(msg, &field);
          return map ? _upb_Map_Size(map) : 0;
        }
      )cc",
      msg_name, resolved_name, FieldInitializer(layout, field));
  output(
      R"cc(
        UPB_INLINE bool $0_$1_get(const $0* msg, $2 key, $3* val) {
          const upb_MiniTableField field = $4;
          const upb_Map* map = upb_Message_GetMap(msg, &field);
          if (!map) return false;
          return _upb_Map_Get(map, &key, $5, val, $6);
        }
      )cc",
      msg_name, resolved_name, MapKeyCType(field), MapValueCType(field),
      FieldInitializer(layout, field), MapKeySize(field, "key"),
      MapValueSize(field, "*val"));
  output(
      R"cc(
        UPB_INLINE $0 $1_$2_next(const $1* msg, size_t* iter) {
          const upb_MiniTableField field = $3;
          const upb_Map* map = upb_Message_GetMap(msg, &field);
          if (!map) return NULL;
          return ($0)_upb_map_next(map, iter);
        }
      )cc",
      CTypeConst(field), msg_name, resolved_name,
      FieldInitializer(layout, field));
}

void GenerateMapEntryGetters(const protobuf::FieldDescriptor* field,
                             absl::string_view msg_name, Output& output) {
  output(
      R"cc(
        UPB_INLINE $0 $1_$2(const $1* msg) {
          $3 ret;
          _upb_msg_map_$2(msg, &ret, $4);
          return ret;
        }
      )cc",
      CTypeConst(field), msg_name, field->name(), CType(field),
      field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
          ? "0"
          : "sizeof(ret)");
}

void GenerateRepeatedGetters(const protobuf::FieldDescriptor* field,
                             const FileLayout& layout,
                             absl::string_view msg_name,
                             const NameToFieldDescriptorMap& field_names,
                             Output& output) {
  output(
      R"cc(
        UPB_INLINE $0 const* $1_$2(const $1* msg, size_t* size) {
          const upb_MiniTableField field = $3;
          const upb_Array* arr = upb_Message_GetArray(msg, &field);
          if (arr) {
            if (size) *size = arr->size;
            return ($0 const*)_upb_array_constptr(arr);
          } else {
            if (size) *size = 0;
            return NULL;
          }
        }
      )cc",
      CTypeConst(field), msg_name, ResolveFieldName(field, field_names),
      FieldInitializer(layout, field));
}

void GenerateScalarGetters(const protobuf::FieldDescriptor* field,
                           const FileLayout& layout, absl::string_view msg_name,
                           const NameToFieldDescriptorMap& field_names,
                           Output& output) {
  std::string field_name = ResolveFieldName(field, field_names);
  output(
      R"cc(
        UPB_INLINE $0 $1_$2(const $1* msg) {
          $0 default_val = $3;
          $0 ret;
          const upb_MiniTableField field = $4;
          _upb_Message_GetNonExtensionField(msg, &field, &default_val, &ret);
          return ret;
        }
      )cc",
      CTypeConst(field), msg_name, field_name, FieldDefault(field),
      FieldInitializer(layout, field));
}

void GenerateGetters(const protobuf::FieldDescriptor* field,
                     const FileLayout& layout, absl::string_view msg_name,
                     const NameToFieldDescriptorMap& field_names,
                     Output& output) {
  if (field->is_map()) {
    GenerateMapGetters(field, layout, msg_name, field_names, output);
  } else if (field->containing_type()->options().map_entry()) {
    GenerateMapEntryGetters(field, msg_name, output);
  } else if (field->is_repeated()) {
    GenerateRepeatedGetters(field, layout, msg_name, field_names, output);
  } else {
    GenerateScalarGetters(field, layout, msg_name, field_names, output);
  }
}

void GenerateMapSetters(const protobuf::FieldDescriptor* field,
                        const FileLayout& layout, absl::string_view msg_name,
                        const NameToFieldDescriptorMap& field_names,
                        Output& output) {
  std::string resolved_name = ResolveFieldName(field, field_names);
  output(
      R"cc(
        UPB_INLINE void $0_$1_clear($0* msg) {
          const upb_MiniTableField field = $2;
          upb_Map* map = (upb_Map*)upb_Message_GetMap(msg, &field);
          if (!map) return;
          _upb_Map_Clear(map);
        }
      )cc",
      msg_name, resolved_name, FieldInitializer(layout, field));
  output(
      R"cc(
        UPB_INLINE bool $0_$1_set($0* msg, $2 key, $3 val, upb_Arena* a) {
          const upb_MiniTableField field = $4;
          upb_Map* map = _upb_MiniTable_GetOrCreateMutableMap(msg, &field, $5, $6, a);
          return _upb_Map_Insert(map, &key, $5, &val, $6, a) !=
                 kUpb_MapInsertStatus_OutOfMemory;
        }
      )cc",
      msg_name, resolved_name, MapKeyCType(field), MapValueCType(field),
      FieldInitializer(layout, field), MapKeySize(field, "key"),
      MapValueSize(field, "val"));
  output(
      R"cc(
        UPB_INLINE bool $0_$1_delete($0* msg, $2 key) {
          const upb_MiniTableField field = $3;
          upb_Map* map = (upb_Map*)upb_Message_GetMap(msg, &field);
          if (!map) return false;
          return _upb_Map_Delete(map, &key, $4, NULL);
        }
      )cc",
      msg_name, resolved_name, MapKeyCType(field),
      FieldInitializer(layout, field), MapKeySize(field, "key"));
  output(
      R"cc(
        UPB_INLINE $0 $1_$2_nextmutable($1* msg, size_t* iter) {
          const upb_MiniTableField field = $3;
          upb_Map* map = (upb_Map*)upb_Message_GetMap(msg, &field);
          if (!map) return NULL;
          return ($0)_upb_map_next(map, iter);
        }
      )cc",
      CType(field), msg_name, resolved_name, FieldInitializer(layout, field));
}

void GenerateRepeatedSetters(const protobuf::FieldDescriptor* field,
                             const FileLayout& layout,
                             absl::string_view msg_name,
                             const NameToFieldDescriptorMap& field_names,
                             Output& output) {
  std::string resolved_name = ResolveFieldName(field, field_names);
  output(
      R"cc(
        UPB_INLINE $0* $1_mutable_$2($1* msg, size_t* size) {
          upb_MiniTableField field = $3;
          upb_Array* arr = upb_Message_GetMutableArray(msg, &field);
          if (arr) {
            if (size) *size = arr->size;
            return ($0*)_upb_array_ptr(arr);
          } else {
            if (size) *size = 0;
            return NULL;
          }
        }
      )cc",
      CType(field), msg_name, resolved_name, FieldInitializer(layout, field));
  output(
      R"cc(
        UPB_INLINE $0* $1_resize_$2($1* msg, size_t size, upb_Arena* arena) {
          upb_MiniTableField field = $3;
          return ($0*)upb_Message_ResizeArray(msg, &field, size, arena);
        }
      )cc",
      CType(field), msg_name, resolved_name, FieldInitializer(layout, field));
  if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    output(
        R"cc(
          UPB_INLINE struct $0* $1_add_$2($1* msg, upb_Arena* arena) {
            upb_MiniTableField field = $4;
            upb_Array* arr = upb_Message_GetOrCreateMutableArray(msg, &field, arena);
            if (!arr || !_upb_Array_ResizeUninitialized(arr, arr->size + 1, arena)) {
              return NULL;
            }
            struct $0* sub = (struct $0*)_upb_Message_New(&$3, arena);
            if (!arr || !sub) return NULL;
            _upb_Array_Set(arr, arr->size - 1, &sub, sizeof(sub));
            return sub;
          }
        )cc",
        MessageName(field->message_type()), msg_name, resolved_name,
        MessageInit(field->message_type()), FieldInitializer(layout, field));
  } else {
    output(
        R"cc(
          UPB_INLINE bool $1_add_$2($1* msg, $0 val, upb_Arena* arena) {
            upb_MiniTableField field = $3;
            upb_Array* arr = upb_Message_GetOrCreateMutableArray(msg, &field, arena);
            if (!arr || !_upb_Array_ResizeUninitialized(arr, arr->size + 1, arena)) {
              return false;
            }
            _upb_Array_Set(arr, arr->size - 1, &val, sizeof(val));
            return true;
          }
        )cc",
        CType(field), msg_name, resolved_name, FieldInitializer(layout, field));
  }
}

void GenerateNonRepeatedSetters(const protobuf::FieldDescriptor* field,
                                const FileLayout& layout,
                                absl::string_view msg_name,
                                const NameToFieldDescriptorMap& field_names,
                                Output& output) {
  if (field == field->containing_type()->map_key()) {
    // Key cannot be mutated.
    return;
  }

  std::string field_name = ResolveFieldName(field, field_names);

  if (field == field->containing_type()->map_value()) {
    output(R"cc(
             UPB_INLINE void $0_set_$1($0 *msg, $2 value) {
               _upb_msg_map_set_value(msg, &value, $3);
             })cc",
           msg_name, field_name, CType(field),
           field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
               ? "0"
               : "sizeof(" + CType(field) + ")");
  } else {
    output(R"cc(
             UPB_INLINE void $0_set_$1($0 *msg, $2 value) {
               const upb_MiniTableField field = $3;
               _upb_Message_SetNonExtensionField(msg, &field, &value);
             })cc",
           msg_name, field_name, CType(field), FieldInitializer(layout, field));
  }

  // Message fields also have a Msg_mutable_foo() accessor that will create
  // the sub-message if it doesn't already exist.
  if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
      !field->containing_type()->options().map_entry()) {
    output(
        R"cc(
          UPB_INLINE struct $0* $1_mutable_$2($1* msg, upb_Arena* arena) {
            struct $0* sub = (struct $0*)$1_$2(msg);
            if (sub == NULL) {
              sub = (struct $0*)_upb_Message_New(&$3, arena);
              if (sub) $1_set_$2(msg, sub);
            }
            return sub;
          }
        )cc",
        MessageName(field->message_type()), msg_name, field_name,
        MessageInit(field->message_type()));
  }
}

void GenerateSetters(const protobuf::FieldDescriptor* field,
                     const FileLayout& layout, absl::string_view msg_name,
                     const NameToFieldDescriptorMap& field_names,
                     Output& output) {
  if (field->is_map()) {
    GenerateMapSetters(field, layout, msg_name, field_names, output);
  } else if (field->is_repeated()) {
    GenerateRepeatedSetters(field, layout, msg_name, field_names, output);
  } else {
    GenerateNonRepeatedSetters(field, layout, msg_name, field_names, output);
  }
}

void GenerateMessageInHeader(const protobuf::Descriptor* message,
                             const FileLayout& layout, Output& output) {
  output("/* $0 */\n\n", message->full_name());
  std::string msg_name = ToCIdent(message->full_name());
  if (!message->options().map_entry()) {
    GenerateMessageFunctionsInHeader(message, output);
  }

  for (int i = 0; i < message->real_oneof_decl_count(); i++) {
    GenerateOneofInHeader(message->oneof_decl(i), layout, msg_name, output);
  }

  auto field_names = CreateFieldNameMap(message);
  for (auto field : FieldNumberOrder(message)) {
    GenerateClear(field, layout, msg_name, field_names, output);
    GenerateGetters(field, layout, msg_name, field_names, output);
    GenerateHazzer(field, layout, msg_name, field_names, output);
  }

  output("\n");

  for (auto field : FieldNumberOrder(message)) {
    GenerateSetters(field, layout, msg_name, field_names, output);
  }

  output("\n");
}

void WriteHeader(const FileLayout& layout, Output& output) {
  const protobuf::FileDescriptor* file = layout.descriptor();
  EmitFileWarning(file, output);
  output(
      "#ifndef $0_UPB_H_\n"
      "#define $0_UPB_H_\n\n"
      "#include \"upb/collections/array_internal.h\"\n"
      "#include \"upb/collections/map_gencode_util.h\"\n"
      "#include \"upb/message/accessors.h\"\n"
      "#include \"upb/message/internal.h\"\n"
      "#include \"upb/mini_table/enum_internal.h\"\n"
      "#include \"upb/wire/decode.h\"\n"
      "#include \"upb/wire/decode_fast.h\"\n"
      "#include \"upb/wire/encode.h\"\n\n",
      ToPreproc(file->name()));

  for (int i = 0; i < file->public_dependency_count(); i++) {
    if (i == 0) {
      output("/* Public Imports. */\n");
    }
    output("#include \"$0\"\n", HeaderFilename(file->public_dependency(i)));
    if (i == file->public_dependency_count() - 1) {
      output("\n");
    }
  }

  output(
      "#include \"upb/port/def.inc\"\n"
      "\n"
      "#ifdef __cplusplus\n"
      "extern \"C\" {\n"
      "#endif\n"
      "\n");

  const std::vector<const protobuf::Descriptor*> this_file_messages =
      SortedMessages(file);
  const std::vector<const protobuf::FieldDescriptor*> this_file_exts =
      SortedExtensions(file);

  // Forward-declare types defined in this file.
  for (auto message : this_file_messages) {
    output("typedef struct $0 $0;\n", ToCIdent(message->full_name()));
  }
  for (auto message : this_file_messages) {
    output("extern const upb_MiniTable $0;\n", MessageInit(message));
  }
  for (auto ext : this_file_exts) {
    output("extern const upb_MiniTableExtension $0;\n", ExtensionLayout(ext));
  }

  // Forward-declare types not in this file, but used as submessages.
  // Order by full name for consistent ordering.
  std::map<std::string, const protobuf::Descriptor*> forward_messages;

  for (auto* message : this_file_messages) {
    for (int i = 0; i < message->field_count(); i++) {
      const protobuf::FieldDescriptor* field = message->field(i);
      if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
          field->file() != field->message_type()->file()) {
        forward_messages[field->message_type()->full_name()] =
            field->message_type();
      }
    }
  }
  for (auto ext : this_file_exts) {
    if (ext->file() != ext->containing_type()->file()) {
      forward_messages[ext->containing_type()->full_name()] =
          ext->containing_type();
    }
  }
  for (const auto& pair : forward_messages) {
    output("struct $0;\n", MessageName(pair.second));
  }
  for (const auto& pair : forward_messages) {
    output("extern const upb_MiniTable $0;\n", MessageInit(pair.second));
  }

  if (!this_file_messages.empty()) {
    output("\n");
  }

  std::vector<const protobuf::EnumDescriptor*> this_file_enums =
      SortedEnums(file);
  std::sort(
      this_file_enums.begin(), this_file_enums.end(),
      [](const protobuf::EnumDescriptor* a, const protobuf::EnumDescriptor* b) {
        return a->full_name() < b->full_name();
      });

  for (auto enumdesc : this_file_enums) {
    output("typedef enum {\n");
    DumpEnumValues(enumdesc, output);
    output("} $0;\n\n", ToCIdent(enumdesc->full_name()));
  }

  output("\n");

  if (file->syntax() == protobuf::FileDescriptor::SYNTAX_PROTO2) {
    for (const auto* enumdesc : this_file_enums) {
      output("extern const upb_MiniTableEnum $0;\n", EnumInit(enumdesc));
    }
  }

  output("\n");
  for (auto message : this_file_messages) {
    GenerateMessageInHeader(message, layout, output);
  }

  for (auto ext : this_file_exts) {
    GenerateExtensionInHeader(ext, layout, output);
  }

  output("extern const upb_MiniTableFile $0;\n\n", FileLayoutName(file));

  if (file->name() ==
      protobuf::FileDescriptorProto::descriptor()->file()->name()) {
    // This is gratuitously inefficient with how many times it rebuilds
    // MessageLayout objects for the same message. But we only do this for one
    // proto (descriptor.proto) so we don't worry about it.
    const protobuf::Descriptor* max32_message = nullptr;
    const protobuf::Descriptor* max64_message = nullptr;
    size_t max32 = 0;
    size_t max64 = 0;
    for (const auto* message : this_file_messages) {
      if (absl::EndsWith(message->name(), "Options")) {
        size_t size32 = layout.GetMiniTable32(message)->size;
        size_t size64 = layout.GetMiniTable64(message)->size;
        if (size32 > max32) {
          max32 = size32;
          max32_message = message;
        }
        if (size64 > max64) {
          max64 = size64;
          max64_message = message;
        }
      }
    }

    output("/* Max size 32 is $0 */\n", max32_message->full_name());
    output("/* Max size 64 is $0 */\n", max64_message->full_name());
    output("#define _UPB_MAXOPT_SIZE UPB_SIZE($0, $1)\n\n", max32, max64);
  }

  output(
      "#ifdef __cplusplus\n"
      "}  /* extern \"C\" */\n"
      "#endif\n"
      "\n"
      "#include \"upb/port/undef.inc\"\n"
      "\n"
      "#endif  /* $0_UPB_H_ */\n",
      ToPreproc(file->name()));
}

typedef std::pair<std::string, uint64_t> TableEntry;

uint64_t GetEncodedTag(const protobuf::FieldDescriptor* field) {
  protobuf::internal::WireFormatLite::WireType wire_type =
      protobuf::internal::WireFormat::WireTypeForField(field);
  uint32_t unencoded_tag =
      protobuf::internal::WireFormatLite::MakeTag(field->number(), wire_type);
  uint8_t tag_bytes[10] = {0};
  protobuf::io::CodedOutputStream::WriteVarint32ToArray(unencoded_tag,
                                                        tag_bytes);
  uint64_t encoded_tag = 0;
  memcpy(&encoded_tag, tag_bytes, sizeof(encoded_tag));
  // TODO: byte-swap for big endian.
  return encoded_tag;
}

int GetTableSlot(const protobuf::FieldDescriptor* field) {
  uint64_t tag = GetEncodedTag(field);
  if (tag > 0x7fff) {
    // Tag must fit within a two-byte varint.
    return -1;
  }
  return (tag & 0xf8) >> 3;
}

bool TryFillTableEntry(const FileLayout& layout,
                       const protobuf::FieldDescriptor* field,
                       TableEntry& ent) {
  const upb_MiniTable* mt = layout.GetMiniTable64(field->containing_type());
  const upb_MiniTableField* mt_f =
      upb_MiniTable_FindFieldByNumber(mt, field->number());
  std::string type = "";
  std::string cardinality = "";
  switch (mt_f->descriptortype) {
    case kUpb_FieldType_Bool:
      type = "b1";
      break;
    case kUpb_FieldType_Enum:
      // We don't have the means to test proto2 enum fields for valid values.
      return false;
    case kUpb_FieldType_Int32:
    case kUpb_FieldType_UInt32:
      type = "v4";
      break;
    case kUpb_FieldType_Int64:
    case kUpb_FieldType_UInt64:
      type = "v8";
      break;
    case kUpb_FieldType_Fixed32:
    case kUpb_FieldType_SFixed32:
    case kUpb_FieldType_Float:
      type = "f4";
      break;
    case kUpb_FieldType_Fixed64:
    case kUpb_FieldType_SFixed64:
    case kUpb_FieldType_Double:
      type = "f8";
      break;
    case kUpb_FieldType_SInt32:
      type = "z4";
      break;
    case kUpb_FieldType_SInt64:
      type = "z8";
      break;
    case kUpb_FieldType_String:
      type = "s";
      break;
    case kUpb_FieldType_Bytes:
      type = "b";
      break;
    case kUpb_FieldType_Message:
      type = "m";
      break;
    default:
      return false;  // Not supported yet.
  }

  switch (upb_FieldMode_Get(mt_f)) {
    case kUpb_FieldMode_Map:
      return false;  // Not supported yet (ever?).
    case kUpb_FieldMode_Array:
      if (mt_f->mode & kUpb_LabelFlags_IsPacked) {
        cardinality = "p";
      } else {
        cardinality = "r";
      }
      break;
    case kUpb_FieldMode_Scalar:
      if (mt_f->presence < 0) {
        cardinality = "o";
      } else {
        cardinality = "s";
      }
      break;
  }

  uint64_t expected_tag = GetEncodedTag(field);

  // Data is:
  //
  //                  48                32                16                 0
  // |--------|--------|--------|--------|--------|--------|--------|--------|
  // |   offset (16)   |case offset (16) |presence| submsg |  exp. tag (16)  |
  // |--------|--------|--------|--------|--------|--------|--------|--------|
  //
  // - |presence| is either hasbit index or field number for oneofs.

  uint64_t data = static_cast<uint64_t>(mt_f->offset) << 48 | expected_tag;

  if (field->is_repeated()) {
    // No hasbit/oneof-related fields.
  }
  if (field->real_containing_oneof()) {
    uint64_t case_offset = ~mt_f->presence;
    if (case_offset > 0xffff) return false;
    assert(field->number() < 256);
    data |= field->number() << 24;
    data |= case_offset << 32;
  } else {
    uint64_t hasbit_index = 63;  // No hasbit (set a high, unused bit).
    if (mt_f->presence) {
      hasbit_index = mt_f->presence;
      if (hasbit_index > 31) return false;
    }
    data |= hasbit_index << 24;
  }

  if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    uint64_t idx = mt_f->submsg_index;
    if (idx > 255) return false;
    data |= idx << 16;

    std::string size_ceil = "max";
    size_t size = SIZE_MAX;
    if (field->message_type()->file() == field->file()) {
      // We can only be guaranteed the size of the sub-message if it is in the
      // same file as us.  We could relax this to increase the speed of
      // cross-file sub-message parsing if we are comfortable requiring that
      // users compile all messages at the same time.
      const upb_MiniTable* sub_mt =
          layout.GetMiniTable64(field->message_type());
      size = sub_mt->size + 8;
    }
    std::vector<size_t> breaks = {64, 128, 192, 256};
    for (auto brk : breaks) {
      if (size <= brk) {
        size_ceil = std::to_string(brk);
        break;
      }
    }
    ent.first = absl::Substitute("upb_p$0$1_$2bt_max$3b", cardinality, type,
                                 expected_tag > 0xff ? "2" : "1", size_ceil);

  } else {
    ent.first = absl::Substitute("upb_p$0$1_$2bt", cardinality, type,
                                 expected_tag > 0xff ? "2" : "1");
  }
  ent.second = data;
  return true;
}

std::vector<TableEntry> FastDecodeTable(const protobuf::Descriptor* message,
                                        const FileLayout& layout) {
  std::vector<TableEntry> table;
  for (const auto field : FieldHotnessOrder(message)) {
    TableEntry ent;
    int slot = GetTableSlot(field);
    // std::cerr << "table slot: " << field->number() << ": " << slot << "\n";
    if (slot < 0) {
      // Tag can't fit in the table.
      continue;
    }
    if (!TryFillTableEntry(layout, field, ent)) {
      // Unsupported field type or offset, hasbit index, etc. doesn't fit.
      continue;
    }
    while ((size_t)slot >= table.size()) {
      size_t size = std::max(static_cast<size_t>(1), table.size() * 2);
      table.resize(size, TableEntry{"_upb_FastDecoder_DecodeGeneric", 0});
    }
    if (table[slot].first != "_upb_FastDecoder_DecodeGeneric") {
      // A hotter field already filled this slot.
      continue;
    }
    table[slot] = ent;
  }
  return table;
}

std::string GetFieldRep(const upb_MiniTableField* field32,
                        const upb_MiniTableField* field64) {
  switch (_upb_MiniTableField_GetRep(field32)) {
    case kUpb_FieldRep_1Byte:
      return "kUpb_FieldRep_1Byte";
      break;
    case kUpb_FieldRep_4Byte: {
      if (_upb_MiniTableField_GetRep(field64) == kUpb_FieldRep_4Byte) {
        return "kUpb_FieldRep_4Byte";
      } else {
        assert(_upb_MiniTableField_GetRep(field64) == kUpb_FieldRep_8Byte);
        return "UPB_SIZE(kUpb_FieldRep_4Byte, kUpb_FieldRep_8Byte)";
      }
      break;
    }
    case kUpb_FieldRep_StringView:
      return "kUpb_FieldRep_StringView";
      break;
    case kUpb_FieldRep_8Byte:
      return "kUpb_FieldRep_8Byte";
      break;
  }
  UPB_UNREACHABLE();
}

std::string GetFieldRep(const FileLayout& layout,
                        const protobuf::FieldDescriptor* field) {
  return GetFieldRep(layout.GetField32(field), layout.GetField64(field));
}

// Returns the field mode as a string initializer.
//
// We could just emit this as a number (and we may yet go in that direction) but
// for now emitting symbolic constants gives this better readability and
// debuggability.
std::string GetModeInit(const upb_MiniTableField* field32,
                        const upb_MiniTableField* field64) {
  std::string ret;
  uint8_t mode32 = field32->mode;
  switch (mode32 & kUpb_FieldMode_Mask) {
    case kUpb_FieldMode_Map:
      ret = "kUpb_FieldMode_Map";
      break;
    case kUpb_FieldMode_Array:
      ret = "kUpb_FieldMode_Array";
      break;
    case kUpb_FieldMode_Scalar:
      ret = "kUpb_FieldMode_Scalar";
      break;
    default:
      break;
  }

  if (mode32 & kUpb_LabelFlags_IsPacked) {
    absl::StrAppend(&ret, " | kUpb_LabelFlags_IsPacked");
  }

  if (mode32 & kUpb_LabelFlags_IsExtension) {
    absl::StrAppend(&ret, " | kUpb_LabelFlags_IsExtension");
  }

  if (mode32 & kUpb_LabelFlags_IsAlternate) {
    absl::StrAppend(&ret, " | kUpb_LabelFlags_IsAlternate");
  }

  absl::StrAppend(&ret, " | (", GetFieldRep(field32, field64),
                  " << kUpb_FieldRep_Shift)");
  return ret;
}

std::string FieldInitializer(const upb_MiniTableField* field64,
                             const upb_MiniTableField* field32) {
  return absl::Substitute(
      "{$0, $1, $2, $3, $4, $5}", field64->number,
      FileLayout::UpbSize(field32->offset, field64->offset),
      FileLayout::UpbSize(field32->presence, field64->presence),
      field64->submsg_index == kUpb_NoSub
          ? "kUpb_NoSub"
          : absl::StrCat(field64->submsg_index).c_str(),
      field64->descriptortype, GetModeInit(field32, field64));
}

std::string FieldInitializer(const FileLayout& layout,
                             const protobuf::FieldDescriptor* field) {
  return FieldInitializer(layout.GetField64(field), layout.GetField32(field));
}

// Writes a single field into a .upb.c source file.
void WriteMessageField(const upb_MiniTableField* field64,
                       const upb_MiniTableField* field32, Output& output) {
  output("  $0,\n", FieldInitializer(field64, field32));
}

// Writes a single message into a .upb.c source file.
void WriteMessage(const protobuf::Descriptor* message, const FileLayout& layout,
                  Output& output, bool fasttable_enabled) {
  std::string msg_name = ToCIdent(message->full_name());
  std::string fields_array_ref = "NULL";
  std::string submsgs_array_ref = "NULL";
  std::string subenums_array_ref = "NULL";
  const upb_MiniTable* mt_32 = layout.GetMiniTable32(message);
  const upb_MiniTable* mt_64 = layout.GetMiniTable64(message);
  std::vector<std::string> subs;

  for (int i = 0; i < mt_64->field_count; i++) {
    const upb_MiniTableField* f = &mt_64->fields[i];
    if (f->submsg_index != kUpb_NoSub) {
      subs.push_back(FilePlatformLayout::GetSub(mt_64->subs[f->submsg_index]));
    }
  }

  if (!subs.empty()) {
    std::string submsgs_array_name = msg_name + "_submsgs";
    submsgs_array_ref = "&" + submsgs_array_name + "[0]";
    output("static const upb_MiniTableSub $0[$1] = {\n", submsgs_array_name,
           subs.size());

    for (const auto& sub : subs) {
      output("  $0,\n", sub);
    }

    output("};\n\n");
  }

  if (mt_64->field_count > 0) {
    std::string fields_array_name = msg_name + "__fields";
    fields_array_ref = "&" + fields_array_name + "[0]";
    output("static const upb_MiniTableField $0[$1] = {\n", fields_array_name,
           mt_64->field_count);
    for (int i = 0; i < mt_64->field_count; i++) {
      WriteMessageField(&mt_64->fields[i], &mt_32->fields[i], output);
    }
    output("};\n\n");
  }

  std::vector<TableEntry> table;
  uint8_t table_mask = -1;

  if (fasttable_enabled) {
    table = FastDecodeTable(message, layout);
  }

  if (table.size() > 1) {
    assert((table.size() & (table.size() - 1)) == 0);
    table_mask = (table.size() - 1) << 3;
  }

  std::string msgext = "kUpb_ExtMode_NonExtendable";

  if (message->extension_range_count()) {
    if (message->options().message_set_wire_format()) {
      msgext = "kUpb_ExtMode_IsMessageSet";
    } else {
      msgext = "kUpb_ExtMode_Extendable";
    }
  }

  output("const upb_MiniTable $0 = {\n", MessageInit(message));
  output("  $0,\n", submsgs_array_ref);
  output("  $0,\n", fields_array_ref);
  output("  $0, $1, $2, $3, $4, $5,\n", layout.GetMessageSize(message),
         mt_64->field_count, msgext, mt_64->dense_below, table_mask,
         mt_64->required_count);
  if (!table.empty()) {
    output("  UPB_FASTTABLE_INIT({\n");
    for (const auto& ent : table) {
      output("    {0x$1, &$0},\n", ent.first,
             absl::StrCat(absl::Hex(ent.second, absl::kZeroPad16)));
    }
    output("  }),\n");
  }
  output("};\n\n");
}

void WriteEnum(const upb_MiniTableEnum* mt, const protobuf::EnumDescriptor* e,
               Output& output) {
  std::string values_init = "{\n";
  uint32_t value_count = (mt->mask_limit / 32) + mt->value_count;
  for (uint32_t i = 0; i < value_count; i++) {
    absl::StrAppend(&values_init, "                0x", absl::Hex(mt->data[i]),
                    ",\n");
  }
  values_init += "    }";

  output(
      R"cc(
        const upb_MiniTableEnum $0 = {
            $1,
            $2,
            $3,
        };
      )cc",
      EnumInit(e), mt->mask_limit, mt->value_count, values_init);
  output("\n");
}

int WriteEnums(const FileLayout& layout, Output& output) {
  const protobuf::FileDescriptor* file = layout.descriptor();

  if (file->syntax() != protobuf::FileDescriptor::SYNTAX_PROTO2) {
    return 0;
  }

  std::vector<const protobuf::EnumDescriptor*> this_file_enums =
      SortedEnums(file);

  for (const auto* e : this_file_enums) {
    WriteEnum(layout.GetEnumTable(e), e, output);
  }

  if (!this_file_enums.empty()) {
    output("static const upb_MiniTableEnum *$0[$1] = {\n", kEnumsInit,
           this_file_enums.size());
    for (const auto* e : this_file_enums) {
      output("  &$0,\n", EnumInit(e));
    }
    output("};\n");
    output("\n");
  }

  return this_file_enums.size();
}

int WriteMessages(const FileLayout& layout, Output& output,
                  bool fasttable_enabled) {
  const protobuf::FileDescriptor* file = layout.descriptor();
  std::vector<const protobuf::Descriptor*> file_messages = SortedMessages(file);

  if (file_messages.empty()) return 0;

  for (auto message : file_messages) {
    WriteMessage(message, layout, output, fasttable_enabled);
  }

  output("static const upb_MiniTable *$0[$1] = {\n", kMessagesInit,
         file_messages.size());
  for (auto message : file_messages) {
    output("  &$0,\n", MessageInit(message));
  }
  output("};\n");
  output("\n");
  return file_messages.size();
}

void WriteExtension(const protobuf::FieldDescriptor* ext,
                    const FileLayout& layout, Output& output) {
  output("$0,\n", FieldInitializer(layout, ext));
  const upb_MiniTableExtension* mt_ext =
      reinterpret_cast<const upb_MiniTableExtension*>(layout.GetField32(ext));
  output("  &$0,\n", reinterpret_cast<const char*>(mt_ext->extendee));
  output("  $0,\n", FilePlatformLayout::GetSub(mt_ext->sub));
}

int WriteExtensions(const FileLayout& layout, Output& output) {
  auto exts = SortedExtensions(layout.descriptor());
  absl::flat_hash_set<const protobuf::Descriptor*> forward_decls;

  if (exts.empty()) return 0;

  // Order by full name for consistent ordering.
  std::map<std::string, const protobuf::Descriptor*> forward_messages;

  for (auto ext : exts) {
    forward_messages[ext->containing_type()->full_name()] =
        ext->containing_type();
    if (ext->message_type()) {
      forward_messages[ext->message_type()->full_name()] = ext->message_type();
    }
  }

  for (const auto& decl : forward_messages) {
    output("extern const upb_MiniTable $0;\n", MessageInit(decl.second));
  }

  for (auto ext : exts) {
    output("const upb_MiniTableExtension $0 = {\n  ", ExtensionLayout(ext));
    WriteExtension(ext, layout, output);
    output("\n};\n");
  }

  output(
      "\n"
      "static const upb_MiniTableExtension *$0[$1] = {\n",
      kExtensionsInit, exts.size());

  for (auto ext : exts) {
    output("  &$0,\n", ExtensionLayout(ext));
  }

  output(
      "};\n"
      "\n");
  return exts.size();
}

// Writes a .upb.cc source file.
void WriteSource(const FileLayout& layout, Output& output,
                 bool fasttable_enabled) {
  const protobuf::FileDescriptor* file = layout.descriptor();
  EmitFileWarning(file, output);

  output(
      "#include <stddef.h>\n"
      "#include \"upb/collections/array_internal.h\"\n"
      "#include \"upb/message/internal.h\"\n"
      "#include \"upb/mini_table/enum_internal.h\"\n"
      "#include \"$0\"\n",
      HeaderFilename(file));

  for (int i = 0; i < file->dependency_count(); i++) {
    output("#include \"$0\"\n", HeaderFilename(file->dependency(i)));
  }

  output(
      "\n"
      "#include \"upb/port/def.inc\"\n"
      "\n");

  int msg_count = WriteMessages(layout, output, fasttable_enabled);
  int ext_count = WriteExtensions(layout, output);
  int enum_count = WriteEnums(layout, output);

  output("const upb_MiniTableFile $0 = {\n", FileLayoutName(file));
  output("  $0,\n", msg_count ? kMessagesInit : "NULL");
  output("  $0,\n", enum_count ? kEnumsInit : "NULL");
  output("  $0,\n", ext_count ? kExtensionsInit : "NULL");
  output("  $0,\n", msg_count);
  output("  $0,\n", enum_count);
  output("  $0,\n", ext_count);
  output("};\n\n");

  output("#include \"upb/port/undef.inc\"\n");
  output("\n");
}

class Generator : public protoc::CodeGenerator {
  ~Generator() override {}
  bool Generate(const protobuf::FileDescriptor* file,
                const std::string& parameter, protoc::GeneratorContext* context,
                std::string* error) const override;
  uint64_t GetSupportedFeatures() const override {
    return FEATURE_PROTO3_OPTIONAL;
  }
};

bool Generator::Generate(const protobuf::FileDescriptor* file,
                         const std::string& parameter,
                         protoc::GeneratorContext* context,
                         std::string* error) const {
  bool fasttable_enabled = false;
  std::vector<std::pair<std::string, std::string>> params;
  google::protobuf::compiler::ParseGeneratorParameter(parameter, &params);

  for (const auto& pair : params) {
    if (pair.first == "fasttable") {
      fasttable_enabled = true;
    } else {
      *error = "Unknown parameter: " + pair.first;
      return false;
    }
  }

  FileLayout layout(file);

  std::unique_ptr<protobuf::io::ZeroCopyOutputStream> h_output_stream(
      context->Open(HeaderFilename(file)));
  Output h_output(h_output_stream.get());
  WriteHeader(layout, h_output);

  std::unique_ptr<protobuf::io::ZeroCopyOutputStream> c_output_stream(
      context->Open(SourceFilename(file)));
  Output c_output(c_output_stream.get());
  WriteSource(layout, c_output, fasttable_enabled);

  return true;
}

}  // namespace
}  // namespace upbc

int main(int argc, char** argv) {
  std::unique_ptr<google::protobuf::compiler::CodeGenerator> generator(
      new upbc::Generator());
  return google::protobuf::compiler::PluginMain(argc, argv, generator.get());
}
