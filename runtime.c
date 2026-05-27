#include "runtime.h"

// ==========================================
// SINGLETONS
// ==========================================

static JsValue _JS_UNDEF  = { .type = JS_UNDEFINED };
static JsValue _JS_NULL   = { .type = JS_NULL };
static JsValue _JS_TRUE   = { .type = JS_BOOL, { .boolean = 1 } };
static JsValue _JS_FALSE  = { .type = JS_BOOL, { .boolean = 0 } };

// ==========================================
// EXCEPTION HANDLING GLOBALS
// ==========================================

jmp_buf js_try_stack[MAX_TRY_DEPTH];
JsValue* js_exception_stack[MAX_TRY_DEPTH];
int js_try_depth = 0;

// ==========================================
// GLOBAL OBJECTS
// ==========================================

JsValue* js_console = NULL;
JsValue* js_math = NULL;
JsValue* js_json = NULL;
JsValue* js_Object_global = NULL;
JsValue* js_Array_global = NULL;
JsValue* js_Date_global = NULL;
JsValue* js_RegExp_global = NULL;
JsValue* js_Map_global = NULL;
JsValue* js_Set_global = NULL;
JsValue* js_Error_global = NULL;
JsValue* js_TypeError_global = NULL;
JsValue* js_RangeError_global = NULL;
JsValue* js_SyntaxError_global = NULL;

// ==========================================
// CONSTRUCTORS
// ==========================================

JsValue* js_undefined(void) { return &_JS_UNDEF; }
JsValue* js_null(void)      { return &_JS_NULL; }
JsValue* js_bool(int b)     { return b ? &_JS_TRUE : &_JS_FALSE; }

JsValue* js_number(double n) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_NUMBER;
    v->number = n;
    return v;
}

JsValue* js_string(const char* s) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_STRING;
    v->string = strdup(s ? s : "");
    return v;
}

JsValue* js_array_new(void) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_ARRAY;
    v->array.capacity = 8;
    v->array.length = 0;
    v->array.items = malloc(sizeof(JsValue*) * 8);
    return v;
}

JsValue* js_object_new(void) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_OBJECT;
    v->object.capacity = 8;
    v->object.length = 0;
    v->object.keys = malloc(sizeof(char*) * 8);
    v->object.values = malloc(sizeof(JsValue*) * 8);
    return v;
}

JsValue* js_function(JsFnPtr fn) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_FUNCTION;
    v->function = fn;
    return v;
}

JsValue* js_date_new(double ms) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_DATE;
    v->number = ms;
    return v;
}

JsValue* js_regex_new(const char* pattern, const char* flags) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_REGEX;
    JsRegex* r = malloc(sizeof(JsRegex));
    r->source = strdup(pattern ? pattern : "");
    r->flags = strdup(flags ? flags : "");
    r->global = strchr(r->flags, 'g') != NULL;
    r->ignoreCase = strchr(r->flags, 'i') != NULL;
    r->multiline = strchr(r->flags, 'm') != NULL;
    r->sticky = strchr(r->flags, 'y') != NULL;
    r->lastIndex = 0;
    int cflags = REG_EXTENDED;
    if (r->ignoreCase) cflags |= REG_ICASE;
    if (r->multiline)  cflags |= REG_NEWLINE;
    if (regcomp(&r->compiled, r->source, cflags) != 0) {
        // best-effort: leave compiled in an unspecified state; .test/.exec will return no match
    }
    v->regex = r;
    return v;
}

JsValue* js_map_new(void) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_MAP;
    JsMap* m = malloc(sizeof(JsMap));
    m->capacity = 8;
    m->length = 0;
    m->keys = malloc(sizeof(JsValue*) * 8);
    m->values = malloc(sizeof(JsValue*) * 8);
    v->map = m;
    return v;
}

JsValue* js_set_new(void) {
    JsValue* v = malloc(sizeof(JsValue));
    v->type = JS_SET;
    v->array.capacity = 8;
    v->array.length = 0;
    v->array.items = malloc(sizeof(JsValue*) * 8);
    return v;
}

JsValue* js_error_new(const char* name, const char* message) {
    JsValue* v = js_object_new();
    js_object_set(v, "name", js_string(name ? name : "Error"));
    js_object_set(v, "message", js_string(message ? message : ""));
    js_object_set(v, "stack", js_string(""));
    return v;
}

// ==========================================
// COERCION
// ==========================================

int js_truthy(JsValue* v) {
    if (!v) return 0;
    switch (v->type) {
        case JS_UNDEFINED: return 0;
        case JS_NULL:      return 0;
        case JS_BOOL:      return v->boolean;
        case JS_NUMBER:    return v->number != 0 && !isnan(v->number);
        case JS_STRING:    return v->string[0] != '\0';
        default:           return 1;
    }
}

double js_to_number(JsValue* v) {
    if (!v) return 0;
    switch (v->type) {
        case JS_UNDEFINED: return NAN;
        case JS_NULL:      return 0;
        case JS_BOOL:      return v->boolean ? 1.0 : 0.0;
        case JS_NUMBER:    return v->number;
        case JS_DATE:      return v->number;
        case JS_STRING: {
            if (v->string[0] == '\0') return 0;
            char* end;
            double d = strtod(v->string, &end);
            while (*end == ' ' || *end == '\t' || *end == '\n') end++;
            if (*end != '\0') return NAN;
            return d;
        }
        default: return NAN;
    }
}

char* js_to_string(JsValue* v) {
    if (!v) return strdup("undefined");
    switch (v->type) {
        case JS_UNDEFINED: return strdup("undefined");
        case JS_NULL:      return strdup("null");
        case JS_BOOL:      return strdup(v->boolean ? "true" : "false");
        case JS_NUMBER: {
            if (isnan(v->number)) return strdup("NaN");
            if (isinf(v->number)) return strdup(v->number > 0 ? "Infinity" : "-Infinity");
            char buf[64];
            if (v->number == (long long)v->number && fabs(v->number) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", (long long)v->number);
            } else {
                snprintf(buf, sizeof(buf), "%.14g", v->number);
            }
            return strdup(buf);
        }
        case JS_STRING: return strdup(v->string);
        case JS_ARRAY: {
            char* result = strdup("");
            size_t rlen = 0, rcap = 64;
            result = realloc(result, rcap);
            result[0] = '\0';
            for (int i = 0; i < v->array.length; i++) {
                char* item = js_to_string(v->array.items[i]);
                size_t ilen = strlen(item);
                size_t need = rlen + ilen + (i > 0 ? 1 : 0) + 1;
                while (need > rcap) { rcap *= 2; result = realloc(result, rcap); }
                if (i > 0) { result[rlen++] = ','; }
                memcpy(result + rlen, item, ilen);
                rlen += ilen;
                result[rlen] = '\0';
                free(item);
            }
            return result;
        }
        case JS_OBJECT:   return strdup("[object Object]");
        case JS_FUNCTION: return strdup("function");
        case JS_DATE: {
            time_t t = (time_t)(v->number / 1000.0);
            struct tm tm;
            gmtime_r(&t, &tm);
            int ms = (int)fmod(v->number, 1000.0);
            if (ms < 0) ms += 1000;
            char buf[64];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
            return strdup(buf);
        }
        case JS_REGEX: {
            size_t n = strlen(v->regex->source) + strlen(v->regex->flags) + 4;
            char* buf = malloc(n);
            snprintf(buf, n, "/%s/%s", v->regex->source, v->regex->flags);
            return buf;
        }
        case JS_MAP: return strdup("[object Map]");
        case JS_SET: return strdup("[object Set]");
    }
    return strdup("");
}

// ==========================================
// ARITHMETIC
// ==========================================

JsValue* js_add(JsValue* a, JsValue* b) {
    if (a->type == JS_STRING || b->type == JS_STRING) {
        char* sa = js_to_string(a);
        char* sb = js_to_string(b);
        char* r = malloc(strlen(sa) + strlen(sb) + 1);
        strcpy(r, sa); strcat(r, sb);
        JsValue* v = js_string(r);
        free(sa); free(sb); free(r);
        return v;
    }
    return js_number(js_to_number(a) + js_to_number(b));
}

JsValue* js_sub(JsValue* a, JsValue* b) { return js_number(js_to_number(a) - js_to_number(b)); }
JsValue* js_mul(JsValue* a, JsValue* b) { return js_number(js_to_number(a) * js_to_number(b)); }
JsValue* js_div(JsValue* a, JsValue* b) { return js_number(js_to_number(a) / js_to_number(b)); }
JsValue* js_mod(JsValue* a, JsValue* b) { return js_number(fmod(js_to_number(a), js_to_number(b))); }
JsValue* js_pow(JsValue* a, JsValue* b) { return js_number(pow(js_to_number(a), js_to_number(b))); }

// ==========================================
// COMPARISON
// ==========================================

JsValue* js_eq(JsValue* a, JsValue* b) {
    if (a->type == b->type) {
        switch (a->type) {
            case JS_UNDEFINED: case JS_NULL: return js_bool(1);
            case JS_BOOL:   return js_bool(a->boolean == b->boolean);
            case JS_NUMBER: return js_bool(a->number == b->number);
            case JS_STRING: return js_bool(strcmp(a->string, b->string) == 0);
            default:        return js_bool(a == b);
        }
    }
    if ((a->type == JS_NULL && b->type == JS_UNDEFINED) ||
        (a->type == JS_UNDEFINED && b->type == JS_NULL))
        return js_bool(1);
    if (a->type == JS_NUMBER && b->type == JS_STRING)
        return js_bool(a->number == js_to_number(b));
    if (a->type == JS_STRING && b->type == JS_NUMBER)
        return js_bool(js_to_number(a) == b->number);
    if (a->type == JS_BOOL)
        return js_eq(js_number(a->boolean), b);
    if (b->type == JS_BOOL)
        return js_eq(a, js_number(b->boolean));
    return js_bool(0);
}

JsValue* js_neq(JsValue* a, JsValue* b)        { return js_bool(!js_truthy(js_eq(a, b))); }
JsValue* js_strict_eq(JsValue* a, JsValue* b) {
    if (a->type != b->type) return js_bool(0);
    switch (a->type) {
        case JS_UNDEFINED: case JS_NULL: return js_bool(1);
        case JS_BOOL:   return js_bool(a->boolean == b->boolean);
        case JS_NUMBER: return js_bool(a->number == b->number);
        case JS_STRING: return js_bool(strcmp(a->string, b->string) == 0);
        default:        return js_bool(a == b);
    }
}
JsValue* js_strict_neq(JsValue* a, JsValue* b) { return js_bool(!js_truthy(js_strict_eq(a, b))); }
JsValue* js_lt(JsValue* a, JsValue* b) {
    if (a->type == JS_STRING && b->type == JS_STRING)
        return js_bool(strcmp(a->string, b->string) < 0);
    return js_bool(js_to_number(a) < js_to_number(b));
}
JsValue* js_gt(JsValue* a, JsValue* b) {
    if (a->type == JS_STRING && b->type == JS_STRING)
        return js_bool(strcmp(a->string, b->string) > 0);
    return js_bool(js_to_number(a) > js_to_number(b));
}
JsValue* js_lte(JsValue* a, JsValue* b) {
    if (a->type == JS_STRING && b->type == JS_STRING)
        return js_bool(strcmp(a->string, b->string) <= 0);
    return js_bool(js_to_number(a) <= js_to_number(b));
}
JsValue* js_gte(JsValue* a, JsValue* b) {
    if (a->type == JS_STRING && b->type == JS_STRING)
        return js_bool(strcmp(a->string, b->string) >= 0);
    return js_bool(js_to_number(a) >= js_to_number(b));
}

// ==========================================
// LOGICAL / UNARY
// ==========================================

JsValue* js_not(JsValue* a)    { return js_bool(!js_truthy(a)); }
JsValue* js_neg(JsValue* a)    { return js_number(-js_to_number(a)); }
JsValue* js_pos(JsValue* a)    { return js_number(js_to_number(a)); }
JsValue* js_bitnot(JsValue* a) { return js_number(~(int32_t)js_to_number(a)); }

JsValue* js_typeof(JsValue* a) {
    if (!a) return js_string("undefined");
    switch (a->type) {
        case JS_UNDEFINED: return js_string("undefined");
        case JS_NULL:      return js_string("object");
        case JS_BOOL:      return js_string("boolean");
        case JS_NUMBER:    return js_string("number");
        case JS_STRING:    return js_string("string");
        case JS_FUNCTION:  return js_string("function");
        default:           return js_string("object");
    }
}

// ==========================================
// BITWISE
// ==========================================

JsValue* js_bitor(JsValue* a, JsValue* b)  { return js_number((int32_t)js_to_number(a) | (int32_t)js_to_number(b)); }
JsValue* js_bitand(JsValue* a, JsValue* b) { return js_number((int32_t)js_to_number(a) & (int32_t)js_to_number(b)); }
JsValue* js_bitxor(JsValue* a, JsValue* b) { return js_number((int32_t)js_to_number(a) ^ (int32_t)js_to_number(b)); }
JsValue* js_shl(JsValue* a, JsValue* b)    { return js_number((int32_t)js_to_number(a) << ((int32_t)js_to_number(b) & 31)); }
JsValue* js_shr(JsValue* a, JsValue* b)    { return js_number((int32_t)js_to_number(a) >> ((int32_t)js_to_number(b) & 31)); }
JsValue* js_ushr(JsValue* a, JsValue* b)   { return js_number((uint32_t)(int32_t)js_to_number(a) >> ((int32_t)js_to_number(b) & 31)); }

// ==========================================
// SPECIAL OPERATORS
// ==========================================

JsValue* js_in(JsValue* key, JsValue* obj) {
    char* k = js_to_string(key);
    if (obj->type == JS_OBJECT) {
        for (int i = 0; i < obj->object.length; i++) {
            if (strcmp(obj->object.keys[i], k) == 0) { free(k); return js_bool(1); }
        }
    }
    if (obj->type == JS_ARRAY) {
        double n = js_to_number(key);
        if (n >= 0 && n < obj->array.length && n == (int)n) { free(k); return js_bool(1); }
        if (strcmp(k, "length") == 0) { free(k); return js_bool(1); }
    }
    free(k);
    return js_bool(0);
}

JsValue* js_instanceof(JsValue* a, JsValue* b) {
    (void)a; (void)b;
    return js_bool(0);
}

// ==========================================
// ARRAY OPERATIONS
// ==========================================

void js_array_push(JsValue* arr, JsValue* val) {
    if (arr->type != JS_ARRAY) return;
    if (arr->array.length >= arr->array.capacity) {
        arr->array.capacity *= 2;
        arr->array.items = realloc(arr->array.items, sizeof(JsValue*) * arr->array.capacity);
    }
    arr->array.items[arr->array.length++] = val;
}

JsValue* js_array_get(JsValue* arr, int idx) {
    if (arr->type != JS_ARRAY || idx < 0 || idx >= arr->array.length)
        return js_undefined();
    return arr->array.items[idx];
}

void js_array_set(JsValue* arr, int idx, JsValue* val) {
    if (arr->type != JS_ARRAY || idx < 0) return;
    while (idx >= arr->array.capacity) {
        arr->array.capacity *= 2;
        arr->array.items = realloc(arr->array.items, sizeof(JsValue*) * arr->array.capacity);
    }
    while (idx >= arr->array.length) {
        arr->array.items[arr->array.length++] = js_undefined();
    }
    arr->array.items[idx] = val;
}

// ==========================================
// OBJECT / MEMBER ACCESS
// ==========================================

JsValue* js_object_get(JsValue* obj, const char* key) {
    if (!obj || !key) return js_undefined();
    if (obj->type == JS_ARRAY) {
        if (strcmp(key, "length") == 0) return js_number(obj->array.length);
        char* end;
        long idx = strtol(key, &end, 10);
        if (*end == '\0' && idx >= 0) return js_array_get(obj, (int)idx);
    }
    if (obj->type == JS_STRING) {
        if (strcmp(key, "length") == 0) return js_number(strlen(obj->string));
        char* end;
        long idx = strtol(key, &end, 10);
        if (*end == '\0' && idx >= 0 && idx < (long)strlen(obj->string)) {
            char buf[2] = { obj->string[idx], '\0' };
            return js_string(buf);
        }
    }
    if (obj->type == JS_REGEX) {
        if (strcmp(key, "source") == 0)     return js_string(obj->regex->source);
        if (strcmp(key, "flags") == 0)      return js_string(obj->regex->flags);
        if (strcmp(key, "global") == 0)     return js_bool(obj->regex->global);
        if (strcmp(key, "ignoreCase") == 0) return js_bool(obj->regex->ignoreCase);
        if (strcmp(key, "multiline") == 0)  return js_bool(obj->regex->multiline);
        if (strcmp(key, "sticky") == 0)     return js_bool(obj->regex->sticky);
        if (strcmp(key, "lastIndex") == 0)  return js_number(obj->regex->lastIndex);
    }
    if (obj->type == JS_MAP) {
        if (strcmp(key, "size") == 0) return js_number(obj->map->length);
    }
    if (obj->type == JS_SET) {
        if (strcmp(key, "size") == 0) return js_number(obj->array.length);
    }
    if (obj->type != JS_OBJECT) return js_undefined();
    for (int i = 0; i < obj->object.length; i++) {
        if (strcmp(obj->object.keys[i], key) == 0)
            return obj->object.values[i];
    }
    return js_undefined();
}

void js_object_set(JsValue* obj, const char* key, JsValue* val) {
    if (!obj || !key) return;
    if (obj->type == JS_ARRAY) {
        char* end;
        long idx = strtol(key, &end, 10);
        if (*end == '\0' && idx >= 0) { js_array_set(obj, (int)idx, val); return; }
    }
    if (obj->type == JS_REGEX) {
        if (strcmp(key, "lastIndex") == 0) {
            obj->regex->lastIndex = (int)js_to_number(val);
        }
        return;
    }
    if (obj->type != JS_OBJECT) return;
    for (int i = 0; i < obj->object.length; i++) {
        if (strcmp(obj->object.keys[i], key) == 0) {
            obj->object.values[i] = val;
            return;
        }
    }
    if (obj->object.length >= obj->object.capacity) {
        obj->object.capacity *= 2;
        obj->object.keys = realloc(obj->object.keys, sizeof(char*) * obj->object.capacity);
        obj->object.values = realloc(obj->object.values, sizeof(JsValue*) * obj->object.capacity);
    }
    obj->object.keys[obj->object.length] = strdup(key);
    obj->object.values[obj->object.length] = val;
    obj->object.length++;
}

JsValue* js_member_get(JsValue* obj, JsValue* key) {
    char* k = js_to_string(key);
    JsValue* r = js_object_get(obj, k);
    free(k);
    return r;
}

void js_member_set(JsValue* obj, JsValue* key, JsValue* val) {
    char* k = js_to_string(key);
    if (obj->type == JS_ARRAY) {
        double n = js_to_number(key);
        if (n == (int)n && n >= 0) { js_array_set(obj, (int)n, val); free(k); return; }
    }
    js_object_set(obj, k, val);
    free(k);
}

int js_delete_prop(JsValue* obj, const char* key) {
    if (obj->type != JS_OBJECT) return 0;
    for (int i = 0; i < obj->object.length; i++) {
        if (strcmp(obj->object.keys[i], key) == 0) {
            free(obj->object.keys[i]);
            for (int j = i; j < obj->object.length - 1; j++) {
                obj->object.keys[j] = obj->object.keys[j + 1];
                obj->object.values[j] = obj->object.values[j + 1];
            }
            obj->object.length--;
            return 1;
        }
    }
    return 1;
}

// ==========================================
// METHOD DISPATCH
// ==========================================

// Forward decls for regex helpers used by string methods below.
int regex_exec_at(JsRegex* r, const char* subject, int offset, regmatch_t* pmatch, size_t nmatch);
JsValue* regex_replace(JsRegex* r, const char* subj, const char* repl, int all);
JsValue* regex_match_all(JsRegex* r, const char* subj);

JsValue* js_method_call(JsValue* obj, const char* method, JsValue** args, int argc) {
    if (!obj) return js_undefined();

    // ---- ARRAY METHODS ----
    if (obj->type == JS_ARRAY) {
        if (strcmp(method, "push") == 0) {
            for (int i = 0; i < argc; i++) js_array_push(obj, args[i]);
            return js_number(obj->array.length);
        }
        if (strcmp(method, "pop") == 0) {
            if (obj->array.length == 0) return js_undefined();
            return obj->array.items[--obj->array.length];
        }
        if (strcmp(method, "shift") == 0) {
            if (obj->array.length == 0) return js_undefined();
            JsValue* first = obj->array.items[0];
            for (int i = 1; i < obj->array.length; i++) obj->array.items[i - 1] = obj->array.items[i];
            obj->array.length--;
            return first;
        }
        if (strcmp(method, "unshift") == 0) {
            for (int i = argc - 1; i >= 0; i--) {
                if (obj->array.length >= obj->array.capacity) {
                    obj->array.capacity *= 2;
                    obj->array.items = realloc(obj->array.items, sizeof(JsValue*) * obj->array.capacity);
                }
                for (int j = obj->array.length; j > 0; j--) obj->array.items[j] = obj->array.items[j - 1];
                obj->array.items[0] = args[i];
                obj->array.length++;
            }
            return js_number(obj->array.length);
        }
        if (strcmp(method, "indexOf") == 0 && argc > 0) {
            for (int i = 0; i < obj->array.length; i++)
                if (js_truthy(js_strict_eq(obj->array.items[i], args[0]))) return js_number(i);
            return js_number(-1);
        }
        if (strcmp(method, "includes") == 0 && argc > 0) {
            for (int i = 0; i < obj->array.length; i++)
                if (js_truthy(js_strict_eq(obj->array.items[i], args[0]))) return js_bool(1);
            return js_bool(0);
        }
        if (strcmp(method, "join") == 0) {
            char* sep = argc > 0 ? js_to_string(args[0]) : strdup(",");
            char* result = strdup("");
            size_t rlen = 0, rcap = 64;
            result = realloc(result, rcap);
            int slen = strlen(sep);
            for (int i = 0; i < obj->array.length; i++) {
                char* item = js_to_string(obj->array.items[i]);
                size_t ilen = strlen(item);
                size_t need = rlen + ilen + slen + 1;
                while (need > rcap) { rcap *= 2; result = realloc(result, rcap); }
                if (i > 0) { memcpy(result + rlen, sep, slen); rlen += slen; }
                memcpy(result + rlen, item, ilen); rlen += ilen;
                result[rlen] = '\0';
                free(item);
            }
            JsValue* v = js_string(result);
            free(result); free(sep);
            return v;
        }
        if (strcmp(method, "reverse") == 0) {
            for (int i = 0; i < obj->array.length / 2; i++) {
                JsValue* t = obj->array.items[i];
                obj->array.items[i] = obj->array.items[obj->array.length - 1 - i];
                obj->array.items[obj->array.length - 1 - i] = t;
            }
            return obj;
        }
        if (strcmp(method, "slice") == 0) {
            int start = argc > 0 ? (int)js_to_number(args[0]) : 0;
            int end = argc > 1 ? (int)js_to_number(args[1]) : obj->array.length;
            if (start < 0) start += obj->array.length;
            if (end < 0) end += obj->array.length;
            if (start < 0) start = 0;
            if (end > obj->array.length) end = obj->array.length;
            JsValue* r = js_array_new();
            for (int i = start; i < end; i++) js_array_push(r, obj->array.items[i]);
            return r;
        }
        if (strcmp(method, "splice") == 0) {
            int start = argc > 0 ? (int)js_to_number(args[0]) : 0;
            int delCount = argc > 1 ? (int)js_to_number(args[1]) : obj->array.length - start;
            if (start < 0) start += obj->array.length;
            if (start < 0) start = 0;
            if (start > obj->array.length) start = obj->array.length;
            if (delCount < 0) delCount = 0;
            if (start + delCount > obj->array.length) delCount = obj->array.length - start;
            JsValue* removed = js_array_new();
            for (int i = 0; i < delCount; i++) js_array_push(removed, obj->array.items[start + i]);
            int insertCount = argc > 2 ? argc - 2 : 0;
            int diff = insertCount - delCount;
            if (diff > 0) {
                while (obj->array.length + diff >= obj->array.capacity) {
                    obj->array.capacity *= 2;
                    obj->array.items = realloc(obj->array.items, sizeof(JsValue*) * obj->array.capacity);
                }
                for (int i = obj->array.length - 1; i >= start + delCount; i--)
                    obj->array.items[i + diff] = obj->array.items[i];
            } else if (diff < 0) {
                for (int i = start + delCount; i < obj->array.length; i++)
                    obj->array.items[i + diff] = obj->array.items[i];
            }
            for (int i = 0; i < insertCount; i++) obj->array.items[start + i] = args[2 + i];
            obj->array.length += diff;
            return removed;
        }
        if (strcmp(method, "concat") == 0) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) js_array_push(r, obj->array.items[i]);
            for (int a = 0; a < argc; a++) {
                if (args[a]->type == JS_ARRAY) {
                    for (int i = 0; i < args[a]->array.length; i++) js_array_push(r, args[a]->array.items[i]);
                } else {
                    js_array_push(r, args[a]);
                }
            }
            return r;
        }
        if (strcmp(method, "flat") == 0) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) {
                if (obj->array.items[i]->type == JS_ARRAY) {
                    for (int j = 0; j < obj->array.items[i]->array.length; j++)
                        js_array_push(r, obj->array.items[i]->array.items[j]);
                } else {
                    js_array_push(r, obj->array.items[i]);
                }
            }
            return r;
        }
        if (strcmp(method, "fill") == 0) {
            JsValue* val = argc > 0 ? args[0] : js_undefined();
            int start = argc > 1 ? (int)js_to_number(args[1]) : 0;
            int end = argc > 2 ? (int)js_to_number(args[2]) : obj->array.length;
            if (start < 0) start += obj->array.length;
            if (end < 0) end += obj->array.length;
            for (int i = start; i < end && i < obj->array.length; i++) obj->array.items[i] = val;
            return obj;
        }
        // Callback-based array methods
        if (strcmp(method, "forEach") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                args[0]->function(cb_args, 3);
            }
            return js_undefined();
        }
        if (strcmp(method, "map") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                js_array_push(r, args[0]->function(cb_args, 3));
            }
            return r;
        }
        if (strcmp(method, "filter") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                if (js_truthy(args[0]->function(cb_args, 3)))
                    js_array_push(r, obj->array.items[i]);
            }
            return r;
        }
        if (strcmp(method, "reduce") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            int start = 0;
            JsValue* acc;
            if (argc > 1) { acc = args[1]; } else { acc = obj->array.items[0]; start = 1; }
            for (int i = start; i < obj->array.length; i++) {
                JsValue* cb_args[] = { acc, obj->array.items[i], js_number(i), obj };
                acc = args[0]->function(cb_args, 4);
            }
            return acc;
        }
        if (strcmp(method, "find") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                if (js_truthy(args[0]->function(cb_args, 3))) return obj->array.items[i];
            }
            return js_undefined();
        }
        if (strcmp(method, "findIndex") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                if (js_truthy(args[0]->function(cb_args, 3))) return js_number(i);
            }
            return js_number(-1);
        }
        if (strcmp(method, "some") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                if (js_truthy(args[0]->function(cb_args, 3))) return js_bool(1);
            }
            return js_bool(0);
        }
        if (strcmp(method, "every") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                if (!js_truthy(args[0]->function(cb_args, 3))) return js_bool(0);
            }
            return js_bool(1);
        }
        if (strcmp(method, "sort") == 0) {
            for (int i = 0; i < obj->array.length - 1; i++) {
                for (int j = 0; j < obj->array.length - 1 - i; j++) {
                    int swap = 0;
                    if (argc > 0 && args[0]->type == JS_FUNCTION) {
                        JsValue* cb_args[] = { obj->array.items[j], obj->array.items[j + 1] };
                        swap = js_to_number(args[0]->function(cb_args, 2)) > 0;
                    } else {
                        char* a = js_to_string(obj->array.items[j]);
                        char* b = js_to_string(obj->array.items[j + 1]);
                        swap = strcmp(a, b) > 0;
                        free(a); free(b);
                    }
                    if (swap) {
                        JsValue* t = obj->array.items[j];
                        obj->array.items[j] = obj->array.items[j + 1];
                        obj->array.items[j + 1] = t;
                    }
                }
            }
            return obj;
        }
        if (strcmp(method, "flatMap") == 0 && argc > 0 && args[0]->type == JS_FUNCTION) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* cb_args[] = { obj->array.items[i], js_number(i), obj };
                JsValue* res = args[0]->function(cb_args, 3);
                if (res->type == JS_ARRAY) {
                    for (int j = 0; j < res->array.length; j++) js_array_push(r, res->array.items[j]);
                } else {
                    js_array_push(r, res);
                }
            }
            return r;
        }
        if (strcmp(method, "keys") == 0) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) js_array_push(r, js_number(i));
            return r;
        }
        if (strcmp(method, "values") == 0) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) js_array_push(r, obj->array.items[i]);
            return r;
        }
        if (strcmp(method, "entries") == 0) {
            JsValue* r = js_array_new();
            for (int i = 0; i < obj->array.length; i++) {
                JsValue* pair = js_array_new();
                js_array_push(pair, js_number(i));
                js_array_push(pair, obj->array.items[i]);
                js_array_push(r, pair);
            }
            return r;
        }
        if (strcmp(method, "toString") == 0) {
            return js_method_call(obj, "join", NULL, 0);
        }
    }

    // ---- STRING METHODS ----
    if (obj->type == JS_STRING) {
        int slen = strlen(obj->string);
        if (strcmp(method, "charAt") == 0) {
            int idx = argc > 0 ? (int)js_to_number(args[0]) : 0;
            if (idx < 0 || idx >= slen) return js_string("");
            char buf[2] = { obj->string[idx], '\0' };
            return js_string(buf);
        }
        if (strcmp(method, "charCodeAt") == 0) {
            int idx = argc > 0 ? (int)js_to_number(args[0]) : 0;
            if (idx < 0 || idx >= slen) return js_number(NAN);
            return js_number((unsigned char)obj->string[idx]);
        }
        if (strcmp(method, "indexOf") == 0 && argc > 0) {
            char* needle = js_to_string(args[0]);
            int from = argc > 1 ? (int)js_to_number(args[1]) : 0;
            if (from < 0) from = 0;
            if (from >= slen) { free(needle); return js_number(strlen(needle) == 0 ? slen : -1); }
            char* found = strstr(obj->string + from, needle);
            int result = found ? (int)(found - obj->string) : -1;
            free(needle);
            return js_number(result);
        }
        if (strcmp(method, "lastIndexOf") == 0 && argc > 0) {
            char* needle = js_to_string(args[0]);
            int nlen = strlen(needle);
            int result = -1;
            for (int i = slen - nlen; i >= 0; i--) {
                if (strncmp(obj->string + i, needle, nlen) == 0) { result = i; break; }
            }
            free(needle);
            return js_number(result);
        }
        if (strcmp(method, "includes") == 0 && argc > 0) {
            char* needle = js_to_string(args[0]);
            int r = strstr(obj->string, needle) != NULL;
            free(needle);
            return js_bool(r);
        }
        if (strcmp(method, "startsWith") == 0 && argc > 0) {
            char* prefix = js_to_string(args[0]);
            int r = strncmp(obj->string, prefix, strlen(prefix)) == 0;
            free(prefix);
            return js_bool(r);
        }
        if (strcmp(method, "endsWith") == 0 && argc > 0) {
            char* suffix = js_to_string(args[0]);
            int sfxlen = strlen(suffix);
            int r = sfxlen <= slen && strcmp(obj->string + slen - sfxlen, suffix) == 0;
            free(suffix);
            return js_bool(r);
        }
        if (strcmp(method, "slice") == 0 || strcmp(method, "substring") == 0) {
            int start = argc > 0 ? (int)js_to_number(args[0]) : 0;
            int end = argc > 1 ? (int)js_to_number(args[1]) : slen;
            if (start < 0) start += slen;
            if (end < 0) end += slen;
            if (start < 0) start = 0;
            if (end > slen) end = slen;
            if (start >= end) return js_string("");
            char* buf = malloc(end - start + 1);
            memcpy(buf, obj->string + start, end - start);
            buf[end - start] = '\0';
            JsValue* v = js_string(buf);
            free(buf);
            return v;
        }
        if (strcmp(method, "toUpperCase") == 0) {
            char* s = strdup(obj->string);
            for (int i = 0; s[i]; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32;
            JsValue* v = js_string(s); free(s); return v;
        }
        if (strcmp(method, "toLowerCase") == 0) {
            char* s = strdup(obj->string);
            for (int i = 0; s[i]; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
            JsValue* v = js_string(s); free(s); return v;
        }
        if (strcmp(method, "trim") == 0) {
            char* s = obj->string;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            int len = strlen(s);
            while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
            char* buf = malloc(len + 1);
            memcpy(buf, s, len); buf[len] = '\0';
            JsValue* v = js_string(buf); free(buf); return v;
        }
        if (strcmp(method, "trimStart") == 0) {
            char* s = obj->string;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            return js_string(s);
        }
        if (strcmp(method, "trimEnd") == 0) {
            char* s = strdup(obj->string);
            int len = strlen(s);
            while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
            s[len] = '\0';
            JsValue* v = js_string(s); free(s); return v;
        }
        if (strcmp(method, "split") == 0) {
            if (argc > 0 && args[0]->type == JS_REGEX) {
                JsValue* arr = js_array_new();
                JsRegex* r = args[0]->regex;
                regmatch_t m[1];
                int offset = 0;
                int sublen = (int)strlen(obj->string);
                while (offset <= sublen && regex_exec_at(r, obj->string, offset, m, 1)) {
                    int so = offset + m[0].rm_so, eo = offset + m[0].rm_eo;
                    int chunk = so - offset;
                    char* part = malloc(chunk + 1);
                    memcpy(part, obj->string + offset, chunk); part[chunk] = '\0';
                    js_array_push(arr, js_string(part));
                    free(part);
                    if (eo == so) offset = so + 1;
                    else offset = eo;
                }
                int rem = sublen - offset;
                char* tail = malloc(rem + 1);
                if (rem > 0) memcpy(tail, obj->string + offset, rem);
                tail[rem < 0 ? 0 : rem] = '\0';
                js_array_push(arr, js_string(tail));
                free(tail);
                return arr;
            }
            char* sep = argc > 0 ? js_to_string(args[0]) : strdup("");
            JsValue* arr = js_array_new();
            int seplen = strlen(sep);
            if (seplen == 0) {
                for (int i = 0; obj->string[i]; i++) {
                    char buf[2] = { obj->string[i], '\0' };
                    js_array_push(arr, js_string(buf));
                }
            } else {
                char* str = strdup(obj->string);
                char* token = str;
                char* found;
                while ((found = strstr(token, sep)) != NULL) {
                    *found = '\0';
                    js_array_push(arr, js_string(token));
                    token = found + seplen;
                }
                js_array_push(arr, js_string(token));
                free(str);
            }
            free(sep);
            return arr;
        }
        if (strcmp(method, "replace") == 0 && argc >= 2) {
            if (args[0]->type == JS_REGEX) {
                extern JsValue* regex_replace(JsRegex*, const char*, const char*, int);
                char* repl = js_to_string(args[1]);
                JsValue* v = regex_replace(args[0]->regex, obj->string, repl, args[0]->regex->global);
                free(repl);
                return v;
            }
            char* pattern = js_to_string(args[0]);
            char* replacement = js_to_string(args[1]);
            char* found = strstr(obj->string, pattern);
            if (!found) { free(pattern); free(replacement); return js_string(obj->string); }
            int plen = strlen(pattern);
            int rlen = strlen(replacement);
            char* buf = malloc(slen - plen + rlen + 1);
            int pos = found - obj->string;
            memcpy(buf, obj->string, pos);
            memcpy(buf + pos, replacement, rlen);
            memcpy(buf + pos + rlen, obj->string + pos + plen, slen - pos - plen + 1);
            JsValue* v = js_string(buf);
            free(buf); free(pattern); free(replacement);
            return v;
        }
        if (strcmp(method, "replaceAll") == 0 && argc >= 2) {
            if (args[0]->type == JS_REGEX) {
                extern JsValue* regex_replace(JsRegex*, const char*, const char*, int);
                char* repl = js_to_string(args[1]);
                JsValue* v = regex_replace(args[0]->regex, obj->string, repl, 1);
                free(repl);
                return v;
            }
            char* pattern = js_to_string(args[0]);
            char* replacement = js_to_string(args[1]);
            int plen = strlen(pattern);
            int rlen = strlen(replacement);
            if (plen == 0) { free(pattern); free(replacement); return js_string(obj->string); }
            size_t cap = slen * 2 + 1;
            char* buf = malloc(cap);
            size_t blen = 0;
            char* src = obj->string;
            while (*src) {
                char* found = strstr(src, pattern);
                if (!found) {
                    size_t rest = strlen(src);
                    while (blen + rest + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + blen, src, rest); blen += rest;
                    break;
                }
                size_t chunk = found - src;
                while (blen + chunk + rlen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + blen, src, chunk); blen += chunk;
                memcpy(buf + blen, replacement, rlen); blen += rlen;
                src = found + plen;
            }
            buf[blen] = '\0';
            JsValue* v = js_string(buf);
            free(buf); free(pattern); free(replacement);
            return v;
        }
        if (strcmp(method, "repeat") == 0 && argc > 0) {
            int count = (int)js_to_number(args[0]);
            if (count <= 0) return js_string("");
            char* buf = malloc(slen * count + 1);
            buf[0] = '\0';
            for (int i = 0; i < count; i++) strcat(buf, obj->string);
            JsValue* v = js_string(buf); free(buf); return v;
        }
        if (strcmp(method, "padStart") == 0 && argc > 0) {
            int target = (int)js_to_number(args[0]);
            if (slen >= target) return js_string(obj->string);
            char* pad = argc > 1 ? js_to_string(args[1]) : strdup(" ");
            int padlen = strlen(pad);
            if (padlen == 0) { free(pad); return js_string(obj->string); }
            char* buf = malloc(target + 1);
            int fill = target - slen;
            for (int i = 0; i < fill; i++) buf[i] = pad[i % padlen];
            memcpy(buf + fill, obj->string, slen + 1);
            JsValue* v = js_string(buf); free(buf); free(pad); return v;
        }
        if (strcmp(method, "padEnd") == 0 && argc > 0) {
            int target = (int)js_to_number(args[0]);
            if (slen >= target) return js_string(obj->string);
            char* pad = argc > 1 ? js_to_string(args[1]) : strdup(" ");
            int padlen = strlen(pad);
            if (padlen == 0) { free(pad); return js_string(obj->string); }
            char* buf = malloc(target + 1);
            memcpy(buf, obj->string, slen);
            for (int i = slen; i < target; i++) buf[i] = pad[(i - slen) % padlen];
            buf[target] = '\0';
            JsValue* v = js_string(buf); free(buf); free(pad); return v;
        }
        if (strcmp(method, "concat") == 0) {
            size_t total = slen;
            for (int i = 0; i < argc; i++) { char* s = js_to_string(args[i]); total += strlen(s); free(s); }
            char* buf = malloc(total + 1);
            strcpy(buf, obj->string);
            for (int i = 0; i < argc; i++) { char* s = js_to_string(args[i]); strcat(buf, s); free(s); }
            JsValue* v = js_string(buf); free(buf); return v;
        }
        if (strcmp(method, "at") == 0 && argc > 0) {
            int idx = (int)js_to_number(args[0]);
            if (idx < 0) idx += slen;
            if (idx < 0 || idx >= slen) return js_undefined();
            char buf[2] = { obj->string[idx], '\0' };
            return js_string(buf);
        }
        if (strcmp(method, "toString") == 0 || strcmp(method, "valueOf") == 0) {
            return js_string(obj->string);
        }
    }

    // ---- NUMBER METHODS ----
    if (obj->type == JS_NUMBER) {
        if (strcmp(method, "toFixed") == 0) {
            int digits = argc > 0 ? (int)js_to_number(args[0]) : 0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%.*f", digits, obj->number);
            return js_string(buf);
        }
        if (strcmp(method, "toString") == 0) {
            if (argc > 0) {
                int base = (int)js_to_number(args[0]);
                if (base == 16) {
                    char buf[32]; snprintf(buf, sizeof(buf), "%x", (unsigned)(int)obj->number);
                    return js_string(buf);
                }
                if (base == 8) {
                    char buf[32]; snprintf(buf, sizeof(buf), "%o", (unsigned)(int)obj->number);
                    return js_string(buf);
                }
                if (base == 2) {
                    unsigned n = (unsigned)(int)obj->number;
                    char buf[33]; int i = 0;
                    if (n == 0) { buf[i++] = '0'; }
                    else { char tmp[33]; int j = 0; while (n) { tmp[j++] = '0' + (n & 1); n >>= 1; } while (j--) buf[i++] = tmp[j]; }
                    buf[i] = '\0';
                    return js_string(buf);
                }
            }
            char* s = js_to_string(obj);
            JsValue* v = js_string(s); free(s); return v;
        }
    }

    // ---- FUNCTION METHODS ----
    if (obj->type == JS_FUNCTION) {
        if (strcmp(method, "call") == 0) {
            return obj->function(args + 1, argc > 1 ? argc - 1 : 0);
        }
        if (strcmp(method, "apply") == 0) {
            if (argc > 1 && args[1]->type == JS_ARRAY) {
                return obj->function(args[1]->array.items, args[1]->array.length);
            }
            return obj->function(NULL, 0);
        }
        if (strcmp(method, "bind") == 0) {
            return obj;
        }
        // statics on built-in constructors (Date.now, Date.parse, Date.UTC, Map.groupBy etc.)
        extern JsValue* js_static_dispatch(JsValue* ctor, const char* method, JsValue** args, int argc);
        JsValue* sv = js_static_dispatch(obj, method, args, argc);
        if (sv) return sv;
    }

    // ---- DATE METHODS ----
    if (obj->type == JS_DATE) {
        extern JsValue* js_date_method(JsValue* obj, const char* method, JsValue** args, int argc);
        return js_date_method(obj, method, args, argc);
    }

    // ---- REGEX METHODS ----
    if (obj->type == JS_REGEX) {
        extern JsValue* js_regex_method(JsValue* obj, const char* method, JsValue** args, int argc);
        return js_regex_method(obj, method, args, argc);
    }

    // ---- MAP METHODS ----
    if (obj->type == JS_MAP) {
        extern JsValue* js_map_method(JsValue* obj, const char* method, JsValue** args, int argc);
        return js_map_method(obj, method, args, argc);
    }

    // ---- SET METHODS ----
    if (obj->type == JS_SET) {
        extern JsValue* js_set_method(JsValue* obj, const char* method, JsValue** args, int argc);
        return js_set_method(obj, method, args, argc);
    }

    // ---- GENERIC: look up as function property on object ----
    if (obj->type == JS_OBJECT) {
        JsValue* fn = js_object_get(obj, method);
        if (fn->type == JS_FUNCTION) {
            return fn->function(args, argc);
        }
    }

    // ---- toString/valueOf fallback ----
    if (strcmp(method, "toString") == 0) {
        char* s = js_to_string(obj);
        JsValue* v = js_string(s); free(s); return v;
    }

    return js_undefined();
}

// ==========================================
// EXCEPTION HANDLING
// ==========================================

void js_throw(JsValue* val) {
    if (js_try_depth > 0) {
        js_try_depth--;
        js_exception_stack[js_try_depth] = val;
        longjmp(js_try_stack[js_try_depth], 1);
    }
    char* msg = js_to_string(val);
    fprintf(stderr, "Uncaught: %s\n", msg);
    free(msg);
    exit(1);
}

// ==========================================
// BUILT-IN FUNCTIONS
// ==========================================

JsValue* js_print(JsValue** args, int argc) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        char* s = js_to_string(args[i]);
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return js_undefined();
}

JsValue* js_console_log(JsValue** args, int argc) {
    return js_print(args, argc);
}

JsValue* js_ask_wrapper(JsValue** args, int argc) {
    if (argc > 0) {
        char* prompt = js_to_string(args[0]);
        printf("%s ", prompt);
        free(prompt);
    }
    fflush(stdout);
    char* buf = malloc(4096);
    if (!buf) return js_string("");
    if (!fgets(buf, 4096, stdin)) { free(buf); return js_string(""); }
    buf[strcspn(buf, "\n")] = '\0';
    JsValue* r = js_string(buf);
    free(buf);
    return r;
}

JsValue* js_execute_wrapper(JsValue** args, int argc) {
    if (argc == 0) return js_string("");
    char* cmd = js_to_string(args[0]);
    FILE* pipe = popen(cmd, "r");
    free(cmd);
    if (!pipe) return js_string("");
    size_t capacity = 1024, size = 0;
    char* result = malloc(capacity);
    if (!result) { pclose(pipe); return js_string(""); }
    result[0] = '\0';
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t len = strlen(buffer);
        if (size + len + 1 > capacity) {
            while (size + len + 1 > capacity) capacity *= 2;
            result = realloc(result, capacity);
            if (!result) { pclose(pipe); return js_string(""); }
        }
        memcpy(result + size, buffer, len);
        size += len;
        result[size] = '\0';
    }
    pclose(pipe);
    if (size > 0 && result[size - 1] == '\n') result[--size] = '\0';
    JsValue* v = js_string(result);
    free(result);
    return v;
}

JsValue* js_parseInt_fn(JsValue** args, int argc) {
    if (argc == 0) return js_number(NAN);
    char* s = js_to_string(args[0]);
    int base = argc > 1 ? (int)js_to_number(args[1]) : 10;
    if (base == 0) base = 10;
    char* end;
    long r = strtol(s, &end, base);
    int valid = (end != s);
    free(s);
    return valid ? js_number(r) : js_number(NAN);
}

JsValue* js_parseFloat_fn(JsValue** args, int argc) {
    if (argc == 0) return js_number(NAN);
    char* s = js_to_string(args[0]);
    char* end;
    double r = strtod(s, &end);
    int valid = (end != s);
    free(s);
    return valid ? js_number(r) : js_number(NAN);
}

JsValue* js_isNaN_fn(JsValue** args, int argc) {
    if (argc == 0) return js_bool(1);
    return js_bool(isnan(js_to_number(args[0])));
}

JsValue* js_isFinite_fn(JsValue** args, int argc) {
    if (argc == 0) return js_bool(0);
    double n = js_to_number(args[0]);
    return js_bool(!isnan(n) && !isinf(n));
}

JsValue* js_String_fn(JsValue** args, int argc) {
    if (argc == 0) return js_string("");
    char* s = js_to_string(args[0]);
    JsValue* v = js_string(s); free(s); return v;
}

JsValue* js_Number_fn(JsValue** args, int argc) {
    if (argc == 0) return js_number(0);
    return js_number(js_to_number(args[0]));
}

JsValue* js_Boolean_fn(JsValue** args, int argc) {
    if (argc == 0) return js_bool(0);
    return js_bool(js_truthy(args[0]));
}

JsValue* js_Array_isArray_fn(JsValue** args, int argc) {
    if (argc == 0) return js_bool(0);
    return js_bool(args[0]->type == JS_ARRAY);
}

JsValue* js_Object_keys_fn(JsValue** args, int argc) {
    JsValue* r = js_array_new();
    if (argc == 0 || args[0]->type != JS_OBJECT) return r;
    for (int i = 0; i < args[0]->object.length; i++)
        js_array_push(r, js_string(args[0]->object.keys[i]));
    return r;
}

JsValue* js_Object_values_fn(JsValue** args, int argc) {
    JsValue* r = js_array_new();
    if (argc == 0 || args[0]->type != JS_OBJECT) return r;
    for (int i = 0; i < args[0]->object.length; i++)
        js_array_push(r, args[0]->object.values[i]);
    return r;
}

JsValue* js_Object_entries_fn(JsValue** args, int argc) {
    JsValue* r = js_array_new();
    if (argc == 0 || args[0]->type != JS_OBJECT) return r;
    for (int i = 0; i < args[0]->object.length; i++) {
        JsValue* pair = js_array_new();
        js_array_push(pair, js_string(args[0]->object.keys[i]));
        js_array_push(pair, args[0]->object.values[i]);
        js_array_push(r, pair);
    }
    return r;
}

JsValue* js_Object_assign_fn(JsValue** args, int argc) {
    if (argc == 0) return js_object_new();
    JsValue* target = args[0];
    for (int a = 1; a < argc; a++) {
        if (args[a]->type == JS_OBJECT) {
            for (int i = 0; i < args[a]->object.length; i++)
                js_object_set(target, args[a]->object.keys[i], args[a]->object.values[i]);
        }
    }
    return target;
}

// ==========================================
// JSON
// ==========================================

static void json_stringify_val(JsValue* val, char** buf, size_t* len, size_t* cap) {
    #define JS_ENSURE(n) do { while (*len + (n) + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); } } while(0)

    switch (val->type) {
        case JS_UNDEFINED: case JS_FUNCTION:
            JS_ENSURE(4); memcpy(*buf + *len, "null", 4); *len += 4; break;
        case JS_NULL:
            JS_ENSURE(4); memcpy(*buf + *len, "null", 4); *len += 4; break;
        case JS_BOOL:
            if (val->boolean) { JS_ENSURE(4); memcpy(*buf + *len, "true", 4); *len += 4; }
            else { JS_ENSURE(5); memcpy(*buf + *len, "false", 5); *len += 5; }
            break;
        case JS_NUMBER: {
            char nbuf[64];
            if (isnan(val->number) || isinf(val->number)) strcpy(nbuf, "null");
            else if (val->number == (long long)val->number && fabs(val->number) < 1e15)
                snprintf(nbuf, sizeof(nbuf), "%lld", (long long)val->number);
            else snprintf(nbuf, sizeof(nbuf), "%.14g", val->number);
            size_t nl = strlen(nbuf);
            JS_ENSURE(nl); memcpy(*buf + *len, nbuf, nl); *len += nl;
            break;
        }
        case JS_STRING: {
            JS_ENSURE(1); (*buf)[(*len)++] = '"';
            for (int i = 0; val->string[i]; i++) {
                JS_ENSURE(2);
                switch (val->string[i]) {
                    case '"':  (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '"'; break;
                    case '\\': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '\\'; break;
                    case '\n': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n'; break;
                    case '\r': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'r'; break;
                    case '\t': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 't'; break;
                    default:   (*buf)[(*len)++] = val->string[i]; break;
                }
            }
            JS_ENSURE(1); (*buf)[(*len)++] = '"';
            break;
        }
        case JS_ARRAY:
            JS_ENSURE(1); (*buf)[(*len)++] = '[';
            for (int i = 0; i < val->array.length; i++) {
                if (i > 0) { JS_ENSURE(1); (*buf)[(*len)++] = ','; }
                json_stringify_val(val->array.items[i], buf, len, cap);
            }
            JS_ENSURE(1); (*buf)[(*len)++] = ']';
            break;
        case JS_OBJECT:
            JS_ENSURE(1); (*buf)[(*len)++] = '{';
            for (int i = 0; i < val->object.length; i++) {
                if (val->object.values[i]->type == JS_UNDEFINED || val->object.values[i]->type == JS_FUNCTION) continue;
                if (i > 0) { JS_ENSURE(1); (*buf)[(*len)++] = ','; }
                JS_ENSURE(1); (*buf)[(*len)++] = '"';
                size_t kl = strlen(val->object.keys[i]);
                JS_ENSURE(kl); memcpy(*buf + *len, val->object.keys[i], kl); *len += kl;
                JS_ENSURE(2); (*buf)[(*len)++] = '"'; (*buf)[(*len)++] = ':';
                json_stringify_val(val->object.values[i], buf, len, cap);
            }
            JS_ENSURE(1); (*buf)[(*len)++] = '}';
            break;
        case JS_DATE: {
            char* s = js_to_string(val);
            size_t sl = strlen(s);
            JS_ENSURE(sl + 2);
            (*buf)[(*len)++] = '"';
            memcpy(*buf + *len, s, sl); *len += sl;
            (*buf)[(*len)++] = '"';
            free(s);
            break;
        }
        case JS_REGEX: case JS_MAP: case JS_SET:
            JS_ENSURE(2); memcpy(*buf + *len, "{}", 2); *len += 2; break;
    }
    #undef JS_ENSURE
}

JsValue* js_JSON_stringify_fn(JsValue** args, int argc) {
    if (argc == 0) return js_undefined();
    size_t cap = 256, len = 0;
    char* buf = malloc(cap);
    json_stringify_val(args[0], &buf, &len, &cap);
    buf[len] = '\0';
    JsValue* v = js_string(buf);
    free(buf);
    return v;
}

// JSON parser
static void json_skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static JsValue* json_parse_value(const char** p);

static JsValue* json_parse_string(const char** p) {
    if (**p != '"') return js_string("");
    (*p)++;
    size_t cap = 64, len = 0;
    char* buf = malloc(cap);
    while (**p && **p != '"') {
        if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case '"': buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/': buf[len++] = '/'; break;
                case 'n': buf[len++] = '\n'; break;
                case 'r': buf[len++] = '\r'; break;
                case 't': buf[len++] = '\t'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'f': buf[len++] = '\f'; break;
                default: buf[len++] = **p; break;
            }
        } else {
            buf[len++] = **p;
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    buf[len] = '\0';
    JsValue* v = js_string(buf);
    free(buf);
    return v;
}

static JsValue* json_parse_value(const char** p) {
    json_skip_ws(p);
    if (**p == '"') return json_parse_string(p);
    if (**p == '[') {
        (*p)++;
        JsValue* arr = js_array_new();
        json_skip_ws(p);
        if (**p == ']') { (*p)++; return arr; }
        while (1) {
            json_skip_ws(p);
            js_array_push(arr, json_parse_value(p));
            json_skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            break;
        }
        return arr;
    }
    if (**p == '{') {
        (*p)++;
        JsValue* obj = js_object_new();
        json_skip_ws(p);
        if (**p == '}') { (*p)++; return obj; }
        while (1) {
            json_skip_ws(p);
            if (**p != '"') break;
            JsValue* key = json_parse_string(p);
            json_skip_ws(p);
            if (**p == ':') (*p)++;
            json_skip_ws(p);
            JsValue* val = json_parse_value(p);
            js_object_set(obj, key->string, val);
            json_skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            break;
        }
        return obj;
    }
    if (**p == 't' && strncmp(*p, "true", 4) == 0)  { *p += 4; return js_bool(1); }
    if (**p == 'f' && strncmp(*p, "false", 5) == 0) { *p += 5; return js_bool(0); }
    if (**p == 'n' && strncmp(*p, "null", 4) == 0)  { *p += 4; return js_null(); }
    if (**p == '-' || (**p >= '0' && **p <= '9')) {
        char* end;
        double d = strtod(*p, &end);
        *p = end;
        return js_number(d);
    }
    return js_undefined();
}

JsValue* js_JSON_parse_fn(JsValue** args, int argc) {
    if (argc == 0) return js_undefined();
    char* s = js_to_string(args[0]);
    const char* p = s;
    JsValue* result = json_parse_value(&p);
    free(s);
    return result;
}

// ==========================================
// MATH HELPERS
// ==========================================

static JsValue* js_math_floor(JsValue** a, int c) { return js_number(floor(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_ceil(JsValue** a, int c)  { return js_number(ceil(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_round(JsValue** a, int c) { return js_number(round(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_abs(JsValue** a, int c)   { return js_number(fabs(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_sqrt(JsValue** a, int c)  { return js_number(sqrt(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_cbrt(JsValue** a, int c)  { return js_number(cbrt(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_log(JsValue** a, int c)   { return js_number(log(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_log2(JsValue** a, int c)  { return js_number(log2(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_log10(JsValue** a, int c) { return js_number(log10(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_sin(JsValue** a, int c)   { return js_number(sin(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_cos(JsValue** a, int c)   { return js_number(cos(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_tan(JsValue** a, int c)   { return js_number(tan(c > 0 ? js_to_number(a[0]) : 0)); }
static JsValue* js_math_atan2(JsValue** a, int c) {
    return js_number(atan2(c > 0 ? js_to_number(a[0]) : 0, c > 1 ? js_to_number(a[1]) : 0));
}
static JsValue* js_math_pow_fn(JsValue** a, int c) {
    return js_number(pow(c > 0 ? js_to_number(a[0]) : 0, c > 1 ? js_to_number(a[1]) : 0));
}
static JsValue* js_math_min(JsValue** a, int c) {
    if (c == 0) return js_number(INFINITY);
    double m = js_to_number(a[0]);
    for (int i = 1; i < c; i++) { double v = js_to_number(a[i]); if (v < m) m = v; }
    return js_number(m);
}
static JsValue* js_math_max(JsValue** a, int c) {
    if (c == 0) return js_number(-INFINITY);
    double m = js_to_number(a[0]);
    for (int i = 1; i < c; i++) { double v = js_to_number(a[i]); if (v > m) m = v; }
    return js_number(m);
}
static JsValue* js_math_random(JsValue** a, int c) {
    (void)a; (void)c;
    return js_number((double)rand() / RAND_MAX);
}
static JsValue* js_math_trunc(JsValue** a, int c) {
    return js_number(trunc(c > 0 ? js_to_number(a[0]) : 0));
}
static JsValue* js_math_sign(JsValue** a, int c) {
    double v = c > 0 ? js_to_number(a[0]) : 0;
    if (isnan(v)) return js_number(NAN);
    return js_number(v > 0 ? 1 : v < 0 ? -1 : 0);
}

// console.warn/error -> same as log but to stderr
static JsValue* js_console_warn(JsValue** args, int argc) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        char* s = js_to_string(args[i]);
        fprintf(stderr, "%s", s);
        free(s);
    }
    fprintf(stderr, "\n");
    return js_undefined();
}

// ==========================================
// VALUE EQUALITY (SameValueZero) — used by Map/Set
// ==========================================

static int js_same_value_zero(JsValue* a, JsValue* b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case JS_UNDEFINED:
        case JS_NULL:    return 1;
        case JS_BOOL:    return a->boolean == b->boolean;
        case JS_NUMBER:
        case JS_DATE:
            if (isnan(a->number) && isnan(b->number)) return 1;
            return a->number == b->number;
        case JS_STRING:  return strcmp(a->string, b->string) == 0;
        default:         return a == b;
    }
}

// ==========================================
// DATE
// ==========================================

static double js_date_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static JsValue* js_Date_construct(JsValue** args, int argc) {
    if (argc == 0) return js_date_new(js_date_now_ms());
    if (argc == 1) {
        if (args[0]->type == JS_STRING) {
            struct tm tm = {0};
            char* p = strptime(args[0]->string, "%Y-%m-%dT%H:%M:%S", &tm);
            if (!p) p = strptime(args[0]->string, "%Y-%m-%d", &tm);
            if (!p) return js_date_new(NAN);
            return js_date_new((double)timegm(&tm) * 1000.0);
        }
        if (args[0]->type == JS_DATE) return js_date_new(args[0]->number);
        return js_date_new(js_to_number(args[0]));
    }
    struct tm tm = {0};
    tm.tm_year = (int)js_to_number(args[0]) - 1900;
    tm.tm_mon  = (int)js_to_number(args[1]);
    tm.tm_mday = argc > 2 ? (int)js_to_number(args[2]) : 1;
    tm.tm_hour = argc > 3 ? (int)js_to_number(args[3]) : 0;
    tm.tm_min  = argc > 4 ? (int)js_to_number(args[4]) : 0;
    tm.tm_sec  = argc > 5 ? (int)js_to_number(args[5]) : 0;
    int ms     = argc > 6 ? (int)js_to_number(args[6]) : 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return js_date_new((double)t * 1000.0 + ms);
}

static JsValue* js_Date_now_fn(JsValue** args, int argc) { (void)args; (void)argc; return js_number(js_date_now_ms()); }

static JsValue* js_Date_UTC_fn(JsValue** args, int argc) {
    if (argc == 0) return js_number(NAN);
    struct tm tm = {0};
    tm.tm_year = (int)js_to_number(args[0]) - 1900;
    tm.tm_mon  = argc > 1 ? (int)js_to_number(args[1]) : 0;
    tm.tm_mday = argc > 2 ? (int)js_to_number(args[2]) : 1;
    tm.tm_hour = argc > 3 ? (int)js_to_number(args[3]) : 0;
    tm.tm_min  = argc > 4 ? (int)js_to_number(args[4]) : 0;
    tm.tm_sec  = argc > 5 ? (int)js_to_number(args[5]) : 0;
    int ms     = argc > 6 ? (int)js_to_number(args[6]) : 0;
    return js_number((double)timegm(&tm) * 1000.0 + ms);
}

static JsValue* js_Date_parse_fn(JsValue** args, int argc) {
    if (argc == 0 || args[0]->type != JS_STRING) return js_number(NAN);
    struct tm tm = {0};
    char* p = strptime(args[0]->string, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!p) p = strptime(args[0]->string, "%Y-%m-%d", &tm);
    if (!p) return js_number(NAN);
    return js_number((double)timegm(&tm) * 1000.0);
}

JsValue* js_date_method(JsValue* obj, const char* method, JsValue** args, int argc) {
    (void)args; (void)argc;
    double ms = obj->number;
    if (isnan(ms)) {
        if (strcmp(method, "getTime") == 0 || strcmp(method, "valueOf") == 0) return js_number(NAN);
        if (strcmp(method, "toString") == 0 || strcmp(method, "toISOString") == 0) return js_string("Invalid Date");
        return js_number(NAN);
    }

    if (strcmp(method, "getTime") == 0 || strcmp(method, "valueOf") == 0) return js_number(ms);
    if (strcmp(method, "toISOString") == 0 || strcmp(method, "toJSON") == 0) {
        char* s = js_to_string(obj); JsValue* v = js_string(s); free(s); return v;
    }

    time_t t = (time_t)(ms / 1000.0);
    int msPart = (int)fmod(ms, 1000.0); if (msPart < 0) msPart += 1000;
    struct tm tm;

    // UTC variants
    gmtime_r(&t, &tm);
    if (strcmp(method, "getUTCFullYear") == 0)     return js_number(tm.tm_year + 1900);
    if (strcmp(method, "getUTCMonth") == 0)        return js_number(tm.tm_mon);
    if (strcmp(method, "getUTCDate") == 0)         return js_number(tm.tm_mday);
    if (strcmp(method, "getUTCDay") == 0)          return js_number(tm.tm_wday);
    if (strcmp(method, "getUTCHours") == 0)        return js_number(tm.tm_hour);
    if (strcmp(method, "getUTCMinutes") == 0)      return js_number(tm.tm_min);
    if (strcmp(method, "getUTCSeconds") == 0)      return js_number(tm.tm_sec);
    if (strcmp(method, "getUTCMilliseconds") == 0) return js_number(msPart);
    if (strcmp(method, "getMilliseconds") == 0)    return js_number(msPart);

    localtime_r(&t, &tm);
    if (strcmp(method, "getFullYear") == 0) return js_number(tm.tm_year + 1900);
    if (strcmp(method, "getYear") == 0)     return js_number(tm.tm_year);
    if (strcmp(method, "getMonth") == 0)    return js_number(tm.tm_mon);
    if (strcmp(method, "getDate") == 0)     return js_number(tm.tm_mday);
    if (strcmp(method, "getDay") == 0)      return js_number(tm.tm_wday);
    if (strcmp(method, "getHours") == 0)    return js_number(tm.tm_hour);
    if (strcmp(method, "getMinutes") == 0)  return js_number(tm.tm_min);
    if (strcmp(method, "getSeconds") == 0)  return js_number(tm.tm_sec);
    if (strcmp(method, "getTimezoneOffset") == 0) return js_number(-(double)tm.tm_gmtoff / 60.0);
    if (strcmp(method, "toString") == 0 || strcmp(method, "toDateString") == 0 || strcmp(method, "toLocaleString") == 0) {
        char buf[64];
        strftime(buf, sizeof(buf), "%a %b %d %Y %H:%M:%S", &tm);
        return js_string(buf);
    }
    if (strcmp(method, "toLocaleDateString") == 0) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return js_string(buf);
    }
    if (strcmp(method, "toLocaleTimeString") == 0) {
        char buf[64];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return js_string(buf);
    }
    return js_undefined();
}

// ==========================================
// REGEX
// ==========================================

static JsValue* js_RegExp_construct(JsValue** args, int argc) {
    if (argc == 0) return js_regex_new("(?:)", "");
    char* pat = js_to_string(args[0]);
    char* flg = argc > 1 ? js_to_string(args[1]) : strdup("");
    JsValue* r = js_regex_new(pat, flg);
    free(pat); free(flg);
    return r;
}

// Runs regex against subject starting at offset. Fills pmatch (size nmatch).
// Returns 1 on match, 0 otherwise.
int regex_exec_at(JsRegex* r, const char* subject, int offset, regmatch_t* pmatch, size_t nmatch) {
    int eflags = offset > 0 ? REG_NOTBOL : 0;
    return regexec(&r->compiled, subject + offset, nmatch, pmatch, eflags) == 0;
}

JsValue* js_regex_method(JsValue* obj, const char* method, JsValue** args, int argc) {
    JsRegex* r = obj->regex;
    if (argc == 0) return js_undefined();
    char* subj = js_to_string(args[0]);

    if (strcmp(method, "test") == 0) {
        regmatch_t m[1];
        int hit = regex_exec_at(r, subj, 0, m, 1);
        free(subj);
        return js_bool(hit);
    }

    if (strcmp(method, "exec") == 0) {
        regmatch_t m[32];
        int offset = (r->global || r->sticky) ? r->lastIndex : 0;
        int sublen = (int)strlen(subj);
        if (offset > sublen) { if (r->global || r->sticky) r->lastIndex = 0; free(subj); return js_null(); }
        if (!regex_exec_at(r, subj, offset, m, 32)) {
            if (r->global || r->sticky) r->lastIndex = 0;
            free(subj); return js_null();
        }
        // exec returns an Array-like with extra index/input — modeled as JS_OBJECT
        // so we can attach those properties.
        JsValue* result = js_object_new();
        int matchStart = offset + m[0].rm_so;
        int matchEnd   = offset + m[0].rm_eo;
        int count = 0;
        for (int i = 0; i < 32 && m[i].rm_so != -1; i++) {
            int so = offset + m[i].rm_so, eo = offset + m[i].rm_eo;
            char* part = malloc(eo - so + 1);
            memcpy(part, subj + so, eo - so);
            part[eo - so] = '\0';
            char key[8]; snprintf(key, sizeof(key), "%d", i);
            js_object_set(result, key, js_string(part));
            free(part);
            count++;
        }
        js_object_set(result, "length", js_number(count));
        js_object_set(result, "index",  js_number(matchStart));
        js_object_set(result, "input",  js_string(subj));
        if (r->global || r->sticky) r->lastIndex = matchEnd;
        free(subj);
        return result;
    }

    free(subj);
    return js_undefined();
}

JsValue* regex_match_all(JsRegex* r, const char* subj) {
    JsValue* arr = js_array_new();
    regmatch_t m[1];
    int offset = 0;
    int sublen = (int)strlen(subj);
    while (offset <= sublen && regex_exec_at(r, subj, offset, m, 1)) {
        int so = offset + m[0].rm_so, eo = offset + m[0].rm_eo;
        char* part = malloc(eo - so + 1);
        memcpy(part, subj + so, eo - so);
        part[eo - so] = '\0';
        js_array_push(arr, js_string(part));
        free(part);
        if (eo == so) offset = so + 1;
        else offset = eo;
    }
    return arr;
}

JsValue* regex_replace(JsRegex* r, const char* subj, const char* repl, int all) {
    size_t cap = strlen(subj) * 2 + 64, len = 0;
    char* buf = malloc(cap);
    int offset = 0;
    int sublen = (int)strlen(subj);
    regmatch_t m[32];
    while (offset <= sublen) {
        if (!regex_exec_at(r, subj, offset, m, 32)) break;
        int so = offset + m[0].rm_so, eo = offset + m[0].rm_eo;
        // copy chunk before match
        int chunk = so - offset;
        while (len + chunk + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, subj + offset, chunk); len += chunk;
        // expand replacement
        for (const char* p = repl; *p; p++) {
            if (*p == '$' && *(p+1)) {
                char c = *(p+1);
                if (c == '&') {
                    int gl = eo - so;
                    while (len + gl + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + len, subj + so, gl); len += gl;
                    p++; continue;
                }
                if (c >= '0' && c <= '9') {
                    int gi = c - '0';
                    if (gi < 32 && m[gi].rm_so != -1) {
                        int gso = offset + m[gi].rm_so, geo = offset + m[gi].rm_eo;
                        int gl = geo - gso;
                        while (len + gl + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                        memcpy(buf + len, subj + gso, gl); len += gl;
                    }
                    p++; continue;
                }
                if (c == '$') {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = '$'; p++; continue;
                }
            }
            if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = *p;
        }
        offset = (eo == so) ? so + 1 : eo;
        if (!all) break;
    }
    // copy remainder
    int rem = sublen - offset;
    while (len + rem + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    if (rem > 0) { memcpy(buf + len, subj + offset, rem); len += rem; }
    buf[len] = '\0';
    JsValue* v = js_string(buf);
    free(buf);
    return v;
}

// ==========================================
// MAP
// ==========================================

static int map_find_index(JsMap* m, JsValue* key) {
    for (int i = 0; i < m->length; i++) {
        if (js_same_value_zero(m->keys[i], key)) return i;
    }
    return -1;
}

static JsValue* js_Map_construct(JsValue** args, int argc) {
    JsValue* m = js_map_new();
    if (argc > 0 && args[0]->type == JS_ARRAY) {
        for (int i = 0; i < args[0]->array.length; i++) {
            JsValue* pair = args[0]->array.items[i];
            if (pair->type == JS_ARRAY && pair->array.length >= 2) {
                JsValue* margs[2] = { pair->array.items[0], pair->array.items[1] };
                js_method_call(m, "set", margs, 2);
            }
        }
    }
    return m;
}

JsValue* js_map_method(JsValue* obj, const char* method, JsValue** args, int argc) {
    JsMap* m = obj->map;

    if (strcmp(method, "set") == 0 && argc >= 2) {
        int idx = map_find_index(m, args[0]);
        if (idx >= 0) { m->values[idx] = args[1]; return obj; }
        if (m->length >= m->capacity) {
            m->capacity *= 2;
            m->keys = realloc(m->keys, sizeof(JsValue*) * m->capacity);
            m->values = realloc(m->values, sizeof(JsValue*) * m->capacity);
        }
        m->keys[m->length] = args[0];
        m->values[m->length] = args[1];
        m->length++;
        return obj;
    }
    if (strcmp(method, "get") == 0 && argc >= 1) {
        int idx = map_find_index(m, args[0]);
        return idx >= 0 ? m->values[idx] : js_undefined();
    }
    if (strcmp(method, "has") == 0 && argc >= 1) {
        return js_bool(map_find_index(m, args[0]) >= 0);
    }
    if (strcmp(method, "delete") == 0 && argc >= 1) {
        int idx = map_find_index(m, args[0]);
        if (idx < 0) return js_bool(0);
        for (int i = idx; i < m->length - 1; i++) {
            m->keys[i] = m->keys[i + 1];
            m->values[i] = m->values[i + 1];
        }
        m->length--;
        return js_bool(1);
    }
    if (strcmp(method, "clear") == 0) {
        m->length = 0;
        return js_undefined();
    }
    if (strcmp(method, "keys") == 0) {
        JsValue* a = js_array_new();
        for (int i = 0; i < m->length; i++) js_array_push(a, m->keys[i]);
        return a;
    }
    if (strcmp(method, "values") == 0) {
        JsValue* a = js_array_new();
        for (int i = 0; i < m->length; i++) js_array_push(a, m->values[i]);
        return a;
    }
    if (strcmp(method, "entries") == 0) {
        JsValue* a = js_array_new();
        for (int i = 0; i < m->length; i++) {
            JsValue* pair = js_array_new();
            js_array_push(pair, m->keys[i]);
            js_array_push(pair, m->values[i]);
            js_array_push(a, pair);
        }
        return a;
    }
    if (strcmp(method, "forEach") == 0 && argc >= 1 && args[0]->type == JS_FUNCTION) {
        for (int i = 0; i < m->length; i++) {
            JsValue* cbargs[3] = { m->values[i], m->keys[i], obj };
            args[0]->function(cbargs, 3);
        }
        return js_undefined();
    }
    return js_undefined();
}

// ==========================================
// SET
// ==========================================

static int set_index_of(JsValue* set, JsValue* val) {
    for (int i = 0; i < set->array.length; i++) {
        if (js_same_value_zero(set->array.items[i], val)) return i;
    }
    return -1;
}

static JsValue* js_Set_construct(JsValue** args, int argc) {
    JsValue* s = js_set_new();
    if (argc > 0 && args[0]->type == JS_ARRAY) {
        for (int i = 0; i < args[0]->array.length; i++) {
            JsValue* a[1] = { args[0]->array.items[i] };
            js_method_call(s, "add", a, 1);
        }
    }
    return s;
}

JsValue* js_set_method(JsValue* obj, const char* method, JsValue** args, int argc) {
    if (strcmp(method, "add") == 0 && argc >= 1) {
        if (set_index_of(obj, args[0]) < 0) {
            if (obj->array.length >= obj->array.capacity) {
                obj->array.capacity *= 2;
                obj->array.items = realloc(obj->array.items, sizeof(JsValue*) * obj->array.capacity);
            }
            obj->array.items[obj->array.length++] = args[0];
        }
        return obj;
    }
    if (strcmp(method, "has") == 0 && argc >= 1) {
        return js_bool(set_index_of(obj, args[0]) >= 0);
    }
    if (strcmp(method, "delete") == 0 && argc >= 1) {
        int idx = set_index_of(obj, args[0]);
        if (idx < 0) return js_bool(0);
        for (int i = idx; i < obj->array.length - 1; i++)
            obj->array.items[i] = obj->array.items[i + 1];
        obj->array.length--;
        return js_bool(1);
    }
    if (strcmp(method, "clear") == 0) {
        obj->array.length = 0;
        return js_undefined();
    }
    if (strcmp(method, "values") == 0 || strcmp(method, "keys") == 0) {
        JsValue* a = js_array_new();
        for (int i = 0; i < obj->array.length; i++) js_array_push(a, obj->array.items[i]);
        return a;
    }
    if (strcmp(method, "entries") == 0) {
        JsValue* a = js_array_new();
        for (int i = 0; i < obj->array.length; i++) {
            JsValue* pair = js_array_new();
            js_array_push(pair, obj->array.items[i]);
            js_array_push(pair, obj->array.items[i]);
            js_array_push(a, pair);
        }
        return a;
    }
    if (strcmp(method, "forEach") == 0 && argc >= 1 && args[0]->type == JS_FUNCTION) {
        for (int i = 0; i < obj->array.length; i++) {
            JsValue* cbargs[3] = { obj->array.items[i], obj->array.items[i], obj };
            args[0]->function(cbargs, 3);
        }
        return js_undefined();
    }
    return js_undefined();
}

// ==========================================
// ERROR
// ==========================================

static JsValue* js_Error_construct(JsValue** args, int argc) {
    char* s = argc > 0 ? js_to_string(args[0]) : strdup("");
    JsValue* v = js_error_new("Error", s); free(s); return v;
}
static JsValue* js_TypeError_construct(JsValue** args, int argc) {
    char* s = argc > 0 ? js_to_string(args[0]) : strdup("");
    JsValue* v = js_error_new("TypeError", s); free(s); return v;
}
static JsValue* js_RangeError_construct(JsValue** args, int argc) {
    char* s = argc > 0 ? js_to_string(args[0]) : strdup("");
    JsValue* v = js_error_new("RangeError", s); free(s); return v;
}
static JsValue* js_SyntaxError_construct(JsValue** args, int argc) {
    char* s = argc > 0 ? js_to_string(args[0]) : strdup("");
    JsValue* v = js_error_new("SyntaxError", s); free(s); return v;
}

// ==========================================
// STATIC DISPATCH for built-in constructors
// ==========================================

JsValue* js_static_dispatch(JsValue* ctor, const char* method, JsValue** args, int argc) {
    if (ctor == js_Date_global) {
        if (strcmp(method, "now") == 0)   return js_Date_now_fn(args, argc);
        if (strcmp(method, "UTC") == 0)   return js_Date_UTC_fn(args, argc);
        if (strcmp(method, "parse") == 0) return js_Date_parse_fn(args, argc);
    }
    return NULL;
}

// ==========================================
// RUNTIME INIT
// ==========================================

void js_runtime_init(void) {
    srand(42);

    // console
    js_console = js_object_new();
    js_object_set(js_console, "log", js_function(js_console_log));
    js_object_set(js_console, "warn", js_function(js_console_warn));
    js_object_set(js_console, "error", js_function(js_console_warn));
    js_object_set(js_console, "info", js_function(js_console_log));

    // Math
    js_math = js_object_new();
    js_object_set(js_math, "floor", js_function(js_math_floor));
    js_object_set(js_math, "ceil", js_function(js_math_ceil));
    js_object_set(js_math, "round", js_function(js_math_round));
    js_object_set(js_math, "abs", js_function(js_math_abs));
    js_object_set(js_math, "sqrt", js_function(js_math_sqrt));
    js_object_set(js_math, "cbrt", js_function(js_math_cbrt));
    js_object_set(js_math, "log", js_function(js_math_log));
    js_object_set(js_math, "log2", js_function(js_math_log2));
    js_object_set(js_math, "log10", js_function(js_math_log10));
    js_object_set(js_math, "sin", js_function(js_math_sin));
    js_object_set(js_math, "cos", js_function(js_math_cos));
    js_object_set(js_math, "tan", js_function(js_math_tan));
    js_object_set(js_math, "atan2", js_function(js_math_atan2));
    js_object_set(js_math, "pow", js_function(js_math_pow_fn));
    js_object_set(js_math, "min", js_function(js_math_min));
    js_object_set(js_math, "max", js_function(js_math_max));
    js_object_set(js_math, "random", js_function(js_math_random));
    js_object_set(js_math, "trunc", js_function(js_math_trunc));
    js_object_set(js_math, "sign", js_function(js_math_sign));
    js_object_set(js_math, "PI", js_number(3.14159265358979323846));
    js_object_set(js_math, "E", js_number(2.71828182845904523536));
    js_object_set(js_math, "SQRT2", js_number(1.41421356237309504880));
    js_object_set(js_math, "LN2", js_number(0.69314718055994530942));
    js_object_set(js_math, "LN10", js_number(2.30258509299404568402));

    // JSON
    js_json = js_object_new();
    js_object_set(js_json, "stringify", js_function(js_JSON_stringify_fn));
    js_object_set(js_json, "parse", js_function(js_JSON_parse_fn));

    // Object
    js_Object_global = js_object_new();
    js_object_set(js_Object_global, "keys", js_function(js_Object_keys_fn));
    js_object_set(js_Object_global, "values", js_function(js_Object_values_fn));
    js_object_set(js_Object_global, "entries", js_function(js_Object_entries_fn));
    js_object_set(js_Object_global, "assign", js_function(js_Object_assign_fn));

    // Array
    js_Array_global = js_object_new();
    js_object_set(js_Array_global, "isArray", js_function(js_Array_isArray_fn));

    // Date, RegExp, Map, Set, Error
    js_Date_global         = js_function(js_Date_construct);
    js_RegExp_global       = js_function(js_RegExp_construct);
    js_Map_global          = js_function(js_Map_construct);
    js_Set_global          = js_function(js_Set_construct);
    js_Error_global        = js_function(js_Error_construct);
    js_TypeError_global    = js_function(js_TypeError_construct);
    js_RangeError_global   = js_function(js_RangeError_construct);
    js_SyntaxError_global  = js_function(js_SyntaxError_construct);
}
