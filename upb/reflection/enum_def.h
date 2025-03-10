/*
 * Copyright (c) 2009-2021, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// IWYU pragma: private, include "upb/reflection/def.h"

#ifndef UPB_REFLECTION_ENUM_DEF_H_
#define UPB_REFLECTION_ENUM_DEF_H_

#include "upb/base/string_view.h"
#include "upb/reflection/common.h"

// Must be last.
#include "upb/port/def.inc"

#ifdef __cplusplus
extern "C" {
#endif

bool upb_EnumDef_CheckNumber(const upb_EnumDef* e, int32_t num);
const upb_MessageDef* upb_EnumDef_ContainingType(const upb_EnumDef* e);
int32_t upb_EnumDef_Default(const upb_EnumDef* e);
const upb_FileDef* upb_EnumDef_File(const upb_EnumDef* e);
const upb_EnumValueDef* upb_EnumDef_FindValueByName(const upb_EnumDef* e,
                                                    const char* name);
const upb_EnumValueDef* upb_EnumDef_FindValueByNameWithSize(
    const upb_EnumDef* e, const char* name, size_t size);
const upb_EnumValueDef* upb_EnumDef_FindValueByNumber(const upb_EnumDef* e,
                                                      int32_t num);
const char* upb_EnumDef_FullName(const upb_EnumDef* e);
bool upb_EnumDef_HasOptions(const upb_EnumDef* e);
bool upb_EnumDef_IsClosed(const upb_EnumDef* e);

// Creates a mini descriptor string for an enum, returns true on success.
bool upb_EnumDef_MiniDescriptorEncode(const upb_EnumDef* e, upb_Arena* a,
                                      upb_StringView* out);

const char* upb_EnumDef_Name(const upb_EnumDef* e);
const google_protobuf_EnumOptions* upb_EnumDef_Options(const upb_EnumDef* e);

upb_StringView upb_EnumDef_ReservedName(const upb_EnumDef* e, int i);
int upb_EnumDef_ReservedNameCount(const upb_EnumDef* e);

const upb_EnumReservedRange* upb_EnumDef_ReservedRange(const upb_EnumDef* e,
                                                       int i);
int upb_EnumDef_ReservedRangeCount(const upb_EnumDef* e);

const upb_EnumValueDef* upb_EnumDef_Value(const upb_EnumDef* e, int i);
int upb_EnumDef_ValueCount(const upb_EnumDef* e);

#ifdef __cplusplus
} /* extern "C" */
#endif

#include "upb/port/undef.inc"

#endif /* UPB_REFLECTION_ENUM_DEF_H_ */
