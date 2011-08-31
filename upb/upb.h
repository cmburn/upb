/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2009 Google Inc.  See LICENSE for details.
 * Author: Josh Haberman <jhaberman@gmail.com>
 *
 * This file contains shared definitions that are widely used across upb.
 */

#ifndef UPB_H_
#define UPB_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>  // only for size_t.
#include <assert.h>
#include <stdarg.h>
#include "descriptor_const.h"
#include "atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

// inline if possible, emit standalone code if required.
#ifndef INLINE
#define INLINE static inline
#endif

#define UPB_MAX(x, y) ((x) > (y) ? (x) : (y))
#define UPB_MIN(x, y) ((x) < (y) ? (x) : (y))
#define UPB_INDEX(base, i, m) (void*)((char*)(base) + ((i)*(m)))

INLINE void nop_printf(const char *fmt, ...) { (void)fmt; }

#ifdef NDEBUG
#define DEBUGPRINTF nop_printf
#else
#define DEBUGPRINTF printf
#endif

// Rounds val up to the next multiple of align.
INLINE size_t upb_align_up(size_t val, size_t align) {
  return val % align == 0 ? val : val + align - (val % align);
}

// The maximum that any submessages can be nested.  Matches proto2's limit.
// At the moment this specifies the size of several statically-sized arrays
// and therefore setting it high will cause more memory to be used.  Will
// be replaced by a runtime-configurable limit and dynamically-resizing arrays.
// TODO: make this a runtime-settable property of upb_handlers.
#define UPB_MAX_NESTING 64

// The maximum number of fields that any one .proto type can have.  Note that
// this is very different than the max field number.  It is hard to imagine a
// scenario where more than 2k fields (each with its own name and field number)
// makes sense.  The .proto file to describe it would be 2000 lines long and
// contain 2000 unique names.
//
// With this limit we can store a has-bit offset in 8 bits (2**8 * 8 = 2048)
// and we can store a value offset in 16 bits, since the maximum message
// size is 16,640 bytes (2**8 has-bits + 2048 * 8-byte value).  Note that
// strings and arrays are not counted in this, only the *pointer* to them is.
// An individual string or array is unaffected by this 16k byte limit.
#define UPB_MAX_FIELDS (2048)

#define UPB_MAX_FIELDNUMBER ((1 << 29) - 1)

// Nested type names are separated by periods.
#define UPB_SYMBOL_SEPARATOR '.'

// The longest chain that mutually-recursive types are allowed to form.  For
// example, this is a type cycle of length 2:
//   message A {
//     B b = 1;
//   }
//   message B {
//     A a = 1;
//   }
#define UPB_MAX_TYPE_CYCLE_LEN 16

// The maximum depth that the type graph can have.  Note that this setting does
// not automatically constrain UPB_MAX_NESTING, because type cycles allow for
// unlimited nesting if we do not limit it.  Many algorithms in upb call
// recursive functions that traverse the type graph, so we must limit this to
// avoid blowing the C stack.
#define UPB_MAX_TYPE_DEPTH 64


/* Fundamental types and type constants. **************************************/

// A list of types as they are encoded on-the-wire.
enum upb_wire_type {
  UPB_WIRE_TYPE_VARINT      = 0,
  UPB_WIRE_TYPE_64BIT       = 1,
  UPB_WIRE_TYPE_DELIMITED   = 2,
  UPB_WIRE_TYPE_START_GROUP = 3,
  UPB_WIRE_TYPE_END_GROUP   = 4,
  UPB_WIRE_TYPE_32BIT       = 5,
};

// Type of a field as defined in a .proto file.  eg. string, int32, etc.  The
// integers that represent this are defined by descriptor.proto.  Note that
// descriptor.proto reserves "0" for errors, and we use it to represent
// exceptional circumstances.
typedef uint8_t upb_fieldtype_t;

// For referencing the type constants tersely.
#define UPB_TYPE(type) GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_TYPE_ ## type
#define UPB_LABEL(type) GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_LABEL_LABEL_ ## type

// Info for a given field type.
typedef struct {
  uint8_t align;
  uint8_t size;
  uint8_t native_wire_type;
  uint8_t inmemory_type;    // For example, INT32, SINT32, and SFIXED32 -> INT32
  char *ctype;
} upb_type_info;

// A static array of info about all of the field types, indexed by type number.
extern const upb_type_info upb_types[];


/* upb_value ******************************************************************/

struct _upb_strref;
struct _upb_fielddef;

// Special constants for the upb_value.type field.  These must not conflict
// with any members of FieldDescriptorProto.Type.
#define UPB_TYPE_ENDGROUP 0
#define UPB_VALUETYPE_FIELDDEF 32
#define UPB_VALUETYPE_PTR 33

// A single .proto value.  The owner must have an out-of-band way of knowing
// the type, so that it knows which union member to use.
typedef struct {
  union {
    uint64_t uint64;
    double _double;
    float _float;
    int32_t int32;
    int64_t int64;
    uint32_t uint32;
    bool _bool;
    struct _upb_strref *strref;
    struct _upb_fielddef *fielddef;
    void *_void;
  } val;

#ifndef NDEBUG
  // In debug mode we carry the value type around also so we can check accesses
  // to be sure the right member is being read.
  char type;
#endif
} upb_value;

#ifdef NDEBUG
#define SET_TYPE(dest, val)
#else
#define SET_TYPE(dest, val) dest = val
#endif

#define UPB_VALUE_ACCESSORS(name, membername, ctype, proto_type) \
  INLINE ctype upb_value_get ## name(upb_value val) { \
    assert(val.type == proto_type); \
    return val.val.membername; \
  } \
  INLINE void upb_value_set ## name(upb_value *val, ctype cval) { \
    SET_TYPE(val->type, proto_type); \
    val->val.membername = cval; \
  }
UPB_VALUE_ACCESSORS(double, _double, double, UPB_TYPE(DOUBLE));
UPB_VALUE_ACCESSORS(float, _float, float, UPB_TYPE(FLOAT));
UPB_VALUE_ACCESSORS(int32, int32, int32_t, UPB_TYPE(INT32));
UPB_VALUE_ACCESSORS(int64, int64, int64_t, UPB_TYPE(INT64));
UPB_VALUE_ACCESSORS(uint32, uint32, uint32_t, UPB_TYPE(UINT32));
UPB_VALUE_ACCESSORS(uint64, uint64, uint64_t, UPB_TYPE(UINT64));
UPB_VALUE_ACCESSORS(bool, _bool, bool, UPB_TYPE(BOOL));
UPB_VALUE_ACCESSORS(strref, strref, struct _upb_strref*, UPB_TYPE(STRING));
UPB_VALUE_ACCESSORS(fielddef, fielddef, struct _upb_fielddef*, UPB_VALUETYPE_FIELDDEF);
UPB_VALUE_ACCESSORS(ptr, _void, void*, UPB_VALUETYPE_PTR);

extern upb_value UPB_NO_VALUE;


/* upb_status *****************************************************************/

enum {
  UPB_OK,          // The operation completed successfully.
  UPB_WOULDBLOCK,  // Stream is nonblocking and the operation would block.
  UPB_ERROR,       // An error occurred.
};

typedef struct {
  const char *name;
  // Writes a NULL-terminated string to "buf" containing an error message for
  // the given error code, returning false if the message was too large to fit.
  bool (*code_to_string)(int code, char *buf, size_t len);
} upb_errorspace;

// TODO: consider adding error space and code, to let ie. errno be stored
// as a proper code, or application-specific error codes.
typedef struct {
  char status;
  int code;   // Can be set to a more specific code (defined by error space).
  upb_errorspace *space;
  const char *str;  // NULL when no message is present.  NULL-terminated.
  char *buf;        // Owned by the status.
  size_t bufsize;
} upb_status;

#define UPB_STATUS_INIT {UPB_OK, 0, NULL, NULL, NULL, 0}

void upb_status_init(upb_status *status);
void upb_status_uninit(upb_status *status);

INLINE bool upb_ok(upb_status *status) { return status->code == UPB_OK; }

void upb_status_clear(upb_status *status);
void upb_status_seterrliteral(upb_status *status, const char *msg);
void upb_status_seterrf(upb_status *s, const char *msg, ...);
void upb_status_setcode(upb_status *s, upb_errorspace *space, int code);
// The returned string is invalidated by any other call into the status.
const char *upb_status_getstr(upb_status *s);
void upb_status_copy(upb_status *to, upb_status *from);

extern upb_errorspace upb_posix_errorspace;
void upb_status_fromerrno(upb_status *status);

// Like vaprintf, but uses *buf (which can be NULL) as a starting point and
// reallocates it only if the new value will not fit.  "size" is updated to
// reflect the allocated size of the buffer.  Returns false on memory alloc
// failure.
int upb_vrprintf(char **buf, size_t *size, size_t ofs,
                 const char *fmt, va_list args);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* UPB_H_ */
