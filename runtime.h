#ifndef RUNTIME_H
#define RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <regex.h>
#include <time.h>

// ==========================================
// TYPE SYSTEM
// ==========================================

typedef enum {
    JS_UNDEFINED,
    JS_NULL,
    JS_BOOL,
    JS_NUMBER,
    JS_STRING,
    JS_ARRAY,
    JS_OBJECT,
    JS_FUNCTION,
    JS_DATE,    // uses .number for ms since epoch
    JS_REGEX,   // uses .regex
    JS_MAP,     // uses .map
    JS_SET      // uses .array; methods enforce uniqueness
} JsType;

typedef struct JsValue JsValue;
typedef JsValue* (*JsFnPtr)(JsValue** args, int argc);

typedef struct {
    JsValue** items;
    int length;
    int capacity;
} JsArray;

typedef struct {
    char** keys;
    JsValue** values;
    int length;
    int capacity;
} JsObject;

typedef struct {
    char* source;
    char* flags;
    regex_t compiled;
    int global;
    int ignoreCase;
    int multiline;
    int sticky;
    int lastIndex;
} JsRegex;

typedef struct {
    JsValue** keys;
    JsValue** values;
    int length;
    int capacity;
} JsMap;

struct JsValue {
    JsType type;
    union {
        int boolean;
        double number;
        char* string;
        JsArray array;
        JsObject object;
        JsFnPtr function;
        JsRegex* regex;
        JsMap* map;
    };
};

// ==========================================
// SHORT-CIRCUIT MACROS (GCC statement exprs)
// ==========================================

#define JS_OR(a, b)      ({ JsValue* _t = (a); js_truthy(_t) ? _t : (b); })
#define JS_AND(a, b)     ({ JsValue* _t = (a); js_truthy(_t) ? (b) : _t; })
#define JS_NULLISH(a, b) ({ JsValue* _t = (a); (_t->type != JS_UNDEFINED && _t->type != JS_NULL) ? _t : (b); })

// ==========================================
// CONSTRUCTORS
// ==========================================

JsValue* js_undefined(void);
JsValue* js_null(void);
JsValue* js_bool(int b);
JsValue* js_number(double n);
JsValue* js_string(const char* s);
JsValue* js_array_new(void);
JsValue* js_object_new(void);
JsValue* js_function(JsFnPtr fn);
JsValue* js_date_new(double ms);
JsValue* js_regex_new(const char* pattern, const char* flags);
JsValue* js_map_new(void);
JsValue* js_set_new(void);
JsValue* js_error_new(const char* name, const char* message);

// ==========================================
// COERCION
// ==========================================

int js_truthy(JsValue* v);
double js_to_number(JsValue* v);
char* js_to_string(JsValue* v);

// ==========================================
// ARITHMETIC OPERATORS
// ==========================================

JsValue* js_add(JsValue* a, JsValue* b);
JsValue* js_sub(JsValue* a, JsValue* b);
JsValue* js_mul(JsValue* a, JsValue* b);
JsValue* js_div(JsValue* a, JsValue* b);
JsValue* js_mod(JsValue* a, JsValue* b);
JsValue* js_pow(JsValue* a, JsValue* b);

// ==========================================
// COMPARISON OPERATORS
// ==========================================

JsValue* js_eq(JsValue* a, JsValue* b);
JsValue* js_neq(JsValue* a, JsValue* b);
JsValue* js_strict_eq(JsValue* a, JsValue* b);
JsValue* js_strict_neq(JsValue* a, JsValue* b);
JsValue* js_lt(JsValue* a, JsValue* b);
JsValue* js_gt(JsValue* a, JsValue* b);
JsValue* js_lte(JsValue* a, JsValue* b);
JsValue* js_gte(JsValue* a, JsValue* b);

// ==========================================
// LOGICAL / UNARY OPERATORS
// ==========================================

JsValue* js_not(JsValue* a);
JsValue* js_neg(JsValue* a);
JsValue* js_pos(JsValue* a);
JsValue* js_typeof(JsValue* a);
JsValue* js_bitnot(JsValue* a);

// ==========================================
// BITWISE OPERATORS
// ==========================================

JsValue* js_bitor(JsValue* a, JsValue* b);
JsValue* js_bitand(JsValue* a, JsValue* b);
JsValue* js_bitxor(JsValue* a, JsValue* b);
JsValue* js_shl(JsValue* a, JsValue* b);
JsValue* js_shr(JsValue* a, JsValue* b);
JsValue* js_ushr(JsValue* a, JsValue* b);

// ==========================================
// SPECIAL OPERATORS
// ==========================================

JsValue* js_in(JsValue* key, JsValue* obj);
JsValue* js_instanceof(JsValue* a, JsValue* b);

// ==========================================
// MEMBER / PROPERTY ACCESS
// ==========================================

JsValue* js_object_get(JsValue* obj, const char* key);
void js_object_set(JsValue* obj, const char* key, JsValue* val);
JsValue* js_member_get(JsValue* obj, JsValue* key);
void js_member_set(JsValue* obj, JsValue* key, JsValue* val);
int js_delete_prop(JsValue* obj, const char* key);

// ==========================================
// ARRAY OPERATIONS
// ==========================================

void js_array_push(JsValue* arr, JsValue* val);
JsValue* js_array_get(JsValue* arr, int idx);
void js_array_set(JsValue* arr, int idx, JsValue* val);

// ==========================================
// METHOD DISPATCH
// ==========================================

JsValue* js_method_call(JsValue* obj, const char* method, JsValue** args, int argc);

// ==========================================
// EXCEPTION HANDLING
// ==========================================

#define MAX_TRY_DEPTH 64
extern jmp_buf js_try_stack[MAX_TRY_DEPTH];
extern JsValue* js_exception_stack[MAX_TRY_DEPTH];
extern int js_try_depth;

void js_throw(JsValue* val);

// ==========================================
// BUILT-IN FUNCTIONS
// ==========================================

JsValue* js_print(JsValue** args, int argc);
JsValue* js_console_log(JsValue** args, int argc);
JsValue* js_ask_wrapper(JsValue** args, int argc);
JsValue* js_execute_wrapper(JsValue** args, int argc);
JsValue* js_parseInt_fn(JsValue** args, int argc);
JsValue* js_parseFloat_fn(JsValue** args, int argc);
JsValue* js_isNaN_fn(JsValue** args, int argc);
JsValue* js_isFinite_fn(JsValue** args, int argc);
JsValue* js_String_fn(JsValue** args, int argc);
JsValue* js_Number_fn(JsValue** args, int argc);
JsValue* js_Boolean_fn(JsValue** args, int argc);
JsValue* js_Array_isArray_fn(JsValue** args, int argc);
JsValue* js_Object_keys_fn(JsValue** args, int argc);
JsValue* js_Object_values_fn(JsValue** args, int argc);
JsValue* js_Object_entries_fn(JsValue** args, int argc);
JsValue* js_Object_assign_fn(JsValue** args, int argc);
JsValue* js_JSON_stringify_fn(JsValue** args, int argc);
JsValue* js_JSON_parse_fn(JsValue** args, int argc);

// ==========================================
// GLOBAL OBJECTS
// ==========================================

extern JsValue* js_console;
extern JsValue* js_math;
extern JsValue* js_json;
extern JsValue* js_Object_global;
extern JsValue* js_Array_global;
extern JsValue* js_Date_global;
extern JsValue* js_RegExp_global;
extern JsValue* js_Map_global;
extern JsValue* js_Set_global;
extern JsValue* js_Error_global;
extern JsValue* js_TypeError_global;
extern JsValue* js_RangeError_global;
extern JsValue* js_SyntaxError_global;

// ==========================================
// RUNTIME INIT
// ==========================================

void js_runtime_init(void);

#endif
