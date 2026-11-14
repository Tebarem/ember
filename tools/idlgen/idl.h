#pragma once
/*
 * idlgen: a WebIDL subset parser and a C glue generator for the Ember bindings (W5a).
 *
 * The parser reads .webidl files into a database of declarations (interfaces, partial interfaces,
 * mixins and includes, namespaces, dictionaries, enums, typedefs, callbacks). idl_resolve merges
 * partials and mixins and resolves named types. idl_gen_file writes the C glue for one input file.
 * Everything is allocated from the database and released together with it (idl_db_free); the tool
 * is short-lived, so nothing is freed piecemeal.
 *
 * Supported: interfaces with inheritance, partial interfaces, mixins and includes, namespaces,
 * dictionaries (members with defaults and required, inheritance), enums, typedefs, callback
 * functions, attributes (readonly, [Reflect], [SameObject], [Replaceable] is parsed only),
 * operations with optional, variadic and default arguments, indexed getters, constants,
 * constructors, [Throws], [Clamp], [EnforceRange], [LegacyNullToEmptyString], [CEReactions] and
 * [NewObject] (ignored at run time). Types: any, object, boolean, the integer types, float and
 * double (with unrestricted), DOMString, USVString, ByteString, nullable types, unions of
 * interfaces and DOMString, sequence/record/Promise (parsed, rejected by the generator when they
 * reach a member).
 *
 * Diagnostics: every error names the file and line ("file.webidl:12: message"). Errors are counted
 * in db->nerrors; the first one is kept in db->first_error for tests.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define IDL_MAX_ERR 256
#define IDL_MAX_SAME_SLOTS 32 /* the wrapper cache of [SameObject] values (bindings: EMBI_BIND_SAME_N) */

/* ------------------------------------------------------------------ growable lists */

#define IDL_LIST(T)                                                                            \
    struct {                                                                                   \
        T *items;                                                                              \
        int n, cap;                                                                            \
    }

/* Grows a list's storage (idl_parse.c). The buffer is tracked, so idl_db_free releases it. */
void idl_list_reserve(void **items, int *cap, int n, size_t elem_size);

#define IDL_PUSH(db, list, value)                                                              \
    do {                                                                                       \
        (void)(db);                                                                            \
        idl_list_reserve((void **)&(list).items, &(list).cap, (list).n, sizeof *(list).items); \
        (list).items[(list).n++] = (value);                                                    \
    } while (0)

/* ------------------------------------------------------------------ types */

typedef enum idl_kind {
    IDL_T_VOID,
    IDL_T_ANY,
    IDL_T_OBJECT,
    IDL_T_BOOLEAN,
    IDL_T_OCTET,
    IDL_T_BYTE,
    IDL_T_SHORT,
    IDL_T_USHORT,
    IDL_T_LONG,
    IDL_T_ULONG,
    IDL_T_LONGLONG,
    IDL_T_ULONGLONG,
    IDL_T_FLOAT,
    IDL_T_DOUBLE,
    IDL_T_DOMSTRING,
    IDL_T_USVSTRING,
    IDL_T_BYTESTRING,
    IDL_T_NAMED,     /* an identifier, resolved to one of the kinds below */
    IDL_T_INTERFACE, /* target is an idl_iface */
    IDL_T_DICT,      /* target is an idl_dict */
    IDL_T_ENUM,      /* target is an idl_enum */
    IDL_T_CALLBACK,  /* target is an idl_callback (or a callback interface, which is an object) */
    IDL_T_SEQUENCE,  /* sub[0] */
    IDL_T_RECORD,    /* sub[0] key, sub[1] value */
    IDL_T_PROMISE,   /* sub[0] */
    IDL_T_UNION,     /* sub[0..nsub) */
} idl_kind;

struct idl_type;
typedef struct idl_type {
    uint8_t kind;      /* idl_kind */
    bool nullable;
    bool unrestricted; /* float or double */
    bool cb_iface; /* IDL_T_CALLBACK: a callback interface (an object), not an idl_callback */
    bool resolved;
    const char *name; /* IDL_T_NAMED, and the identifier of a resolved named type */
    const void *target;
    struct idl_type **sub;
    int nsub;
    int line;
} idl_type;

/* ------------------------------------------------------------------ extended attributes */

typedef struct idl_ext {
    const char *name;
    const char *value; /* nullptr when the attribute has no =value or (args) */
} idl_ext;

typedef struct idl_exts {
    idl_ext *items;
    int n, cap;
} idl_exts;

/* ------------------------------------------------------------------ members */

typedef struct idl_arg {
    const char *name;
    idl_type *type;
    bool optional;
    bool variadic;
    const char *def; /* default value text (literal token text), or nullptr */
    idl_exts ext;
    int line;
} idl_arg;

typedef enum idl_member_kind {
    IDL_M_ATTR,
    IDL_M_OP,
    IDL_M_CONST,
    IDL_M_CTOR,
    IDL_M_ITERABLE, /* parsed only: the generator rejects it */
    IDL_M_STRINGIFIER,
} idl_member_kind;

typedef struct idl_arg_list {
    struct idl_arg *items;
    int n, cap;
} idl_arg_list;

typedef struct idl_member {
    uint8_t kind; /* idl_member_kind */
    bool is_static;
    bool readonly;
    bool throws;
    bool getter; /* an indexed getter (operation) */
    bool sameobject;
    bool reflect;
    bool unforgeable;         // [LegacyUnforgeable]
    bool replaceable;         // [Replaceable]
    bool stringifier;         // stringifier attribute / stringifier operation
    const char *put_forwards; // [PutForwards=name]
    idl_type *key;            // iterable<K,V>: the key type (nullptr for iterable<V>)
    const char *reflect_name; /* [Reflect=name], or nullptr for the lowercased attribute name */
    const char *name;         /* nullptr for an unnamed getter, and for a constructor */
    idl_type *type;           /* attribute type, return type, or constant type */
    idl_arg_list args;
    const char *const_text;   /* constant value text */
    idl_exts ext;
    int line;
    int overload;             /* index among the overloads of its name (set by resolve) */
    int noverloads;           /* overloads of the same name in the same interface */
    bool static_clash;        /* a static op sharing its name with a regular op: its symbols get a _static suffix */
    int slot;                 /* [SameObject] attributes: cache slot in a wrapper (resolve numbers them) */
    struct idl_iface *owner;
} idl_member;

/* ------------------------------------------------------------------ declarations */

typedef struct idl_iface {
    const char *name;
    const char *parent;        /* nullptr when none */
    struct idl_iface *parent_decl;
    IDL_LIST(const char *) includes;
    IDL_LIST(idl_member) members;
    idl_exts ext;
    const char *file;          /* base name of the defining file, without .webidl */
    int line;
    bool mixin;
    bool ns;                   /* namespace */
    bool callback_iface;
    bool has_main;             /* a non-partial definition was seen */
    bool is_partial;           /* this record is a partial or a mixin body (merged by resolve) */
    bool merged;               /* the members of the partials and includes were added */
    struct idl_iface *main;    /* resolve: the record that carries the merged members */
    bool node_derived;         /* resolve: the interface inherits from Node */
    int nslots;                /* resolve: [SameObject] cache slots used by the chain up to here */
    int id;                    /* index among the interfaces that get an id (resolve) */
} idl_iface;

typedef struct idl_dmember {
    const char *name;
    idl_type *type;
    const char *def;           /* default value text, or nullptr */
    bool required;
    int line;
} idl_dmember;

typedef struct idl_dict {
    const char *name;
    const char *parent;
    struct idl_dict *parent_decl;
    IDL_LIST(idl_dmember) members;
    idl_exts ext;
    const char *file;
    int line;
    bool is_partial;
    bool result;               /* the generator: used as a return or attribute type */
} idl_dict;

typedef struct idl_enum {
    const char *name;
    IDL_LIST(const char *) values;
    const char *file;
    int line;
} idl_enum;

typedef struct idl_typedef {
    const char *name;
    idl_type *type;
    int line;
} idl_typedef;

typedef struct idl_callback {
    const char *name;
    idl_type *ret;
    idl_arg_list args;
    const char *file;
    int line;
} idl_callback;

typedef struct idl_db {
    IDL_LIST(idl_iface *) ifaces;
    IDL_LIST(idl_dict *) dicts;
    IDL_LIST(idl_enum *) enums;
    IDL_LIST(idl_typedef *) typedefs;
    IDL_LIST(idl_callback *) callbacks;
    IDL_LIST(const char *) files; /* base names, in the order they were parsed */
    int nerrors;
    char first_error[IDL_MAX_ERR];
    bool resolved;
} idl_db;

/* ------------------------------------------------------------------ API (idl_parse.c) */

/* Parses one file's text. name is the file's base name (without .webidl), used in messages and
 * as the generated file name. Returns the number of errors added by this file. */
int idl_parse(idl_db *db, const char *name, const char *text, size_t len);
/* Merges partial interfaces, dictionaries and mixin includes, resolves named types, numbers the
 * overloads and the interface ids. Returns the number of errors added by this pass. */
int idl_resolve(idl_db *db);
/* Frees everything the database allocated. */
void idl_db_free(idl_db *db);
/* Looks up declarations by name (nullptr when absent). */
idl_iface *idl_find_iface(const idl_db *db, const char *name);
idl_dict *idl_find_dict(const idl_db *db, const char *name);
idl_enum *idl_find_enum(const idl_db *db, const char *name);
/* The overload category of a type (the value of the matching EMBI_UM_* constant of the binding
 * runtime): 0 Node interface, 1 other interface, 2 callback, 3 sequence, 4 object-like (object,
 * dictionary, record), 5 boolean, 6 number, 7 string or enum, 8 any or union. *iface receives the
 * interface id for 0 and 1 (else 0). *nullable is set for nullable types and dictionaries. */
int idl_type_cat(const struct idl_type *t, int *iface, bool *nullable);
/* True when the interface inherits from (or is) Node. */
bool idl_is_node(const idl_iface *i);

/* ------------------------------------------------------------------ generator (idl_gen.c) */

/* Writes the glue of one file: out_c and out_h are the C source and the header of the file's
 * interfaces, dictionaries and enums. Returns the number of errors added (unsupported types). */
int idl_gen_file(idl_db *db, const char *stem, FILE *out_c, FILE *out_h);
/* Writes gen/types.h (ids, classes, shared declarations) and gen/ifaces.c (class and installer
 * tables). Output is sorted by name, so regeneration is deterministic. */
int idl_gen_shared(idl_db *db, FILE *out_types_h, FILE *out_ifaces_c);
/* Writes shapes.c and shapes.h: the native functions shared by the member tables. Call it after
 * every idl_gen_file (the functions are collected while the files are generated); it empties the
 * collection. */
void idl_gen_shapes(FILE *out_c, FILE *out_h);
/* Writes gen/cssprops.c from the text of src/css/props.txt (the names of every property, sorted).
 * The CSSStyleDeclaration accessors are generated from the same list. */
int idl_gen_cssprops(idl_db *db, const char *props_txt, FILE *out_c);
/* The property names read by idl_gen_cssprops, for the interfaces with [CSSProperties]. */
void idl_set_cssprops(const char *props_txt);

/* Diagnostics and extended attributes shared with the generator (idl_parse.c). */
void idl_report(idl_db *db, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
bool idl_has_ext(const idl_exts *e, const char *name);
const idl_ext *idl_ext_get(const idl_exts *e, const char *name);
