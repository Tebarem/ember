/* Module grammar and module unit parsing (J3c). Included by parser.c, after the statements and
 * before the program section, so it shares the parser's helpers.
 *
 * A module unit is parsed with a top function (is_module) whose root scope holds the module's
 * bindings. Its bytecode starts with an init block that makes the boxes of the bindings and
 * instantiates the function declarations, then GEN_START (scope.c), then the body. The body
 * starts at pc 5 with a NOP, which becomes ASYNC_START when the module uses await at its top
 * level. An import binding has no storage: a reference is an IMPORT_GET of a token that names
 * the module holder and the import index. */

typedef struct mod_spec {
    embi_js_value local;
    embi_js_value exported;
    bool local_string;
    bool local_ident;
} mod_spec;

/* An import token: the varray [holder, index]. Its constant is referenced by IMPORT_GET. */
static embi_js_value import_token_new(cunit *p, embi_js_value holder, uint32_t idx)
{
    embi_js_varray *t = embi_js_varray_new(p->rt, 2);
    t->elems[0] = holder;
    t->elems[1] = embi_js_from_int32((int32_t)idx);
    return embi_js_from_cell(t);
}

/* A module specifier or string export name: the atom of a string literal's value. */
static embi_js_value mod_string_atom(cunit *p)
{
    embi_js_string *str = embi_js_str_new_utf16(p->rt, p->tok.cooked, p->tok.cooked_len);
    return embi_js_from_string(embi_js_atom_intern(p->rt, str));
}

/* An export or import name: any IdentifierName, or a string literal that is well-formed Unicode.
 * *was_string tells the two apart. */
static bool mod_name(cunit *p, embi_js_value *out, bool *was_string)
{
    *was_string = false;
    if (is(p, EMBI_JST_STRING)) {
        const uint16_t *c = p->tok.cooked;
        uint32_t n = p->tok.cooked_len;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t u = c[i];
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (i + 1 < n && c[i + 1] >= 0xDC00 && c[i + 1] <= 0xDFFF) {
                    i++;
                    continue;
                }
                return perr(p, "Invalid module export name: contains an unpaired surrogate");
            }
            if (u >= 0xDC00 && u <= 0xDFFF) {
                return perr(p, "Invalid module export name: contains an unpaired surrogate");
            }
        }
        *out = mod_string_atom(p);
        *was_string = true;
        adv(p);
        return true;
    }
    if (is(p, EMBI_JST_NAME)) {
        *out = name_atom(p);
        adv(p);
        return true;
    }
    return unexpected(p);
}

/* A local binding identifier of an import: a BindingIdentifier. */
static bool mod_binding_ident(cunit *p, embi_js_value *out)
{
    if (!is_ident(p)) {
        return unexpected(p);
    }
    *out = name_atom(p);
    if (!check_strict_name(p, *out)) {
        return false;
    }
    adv(p);
    return true;
}

static uint32_t mod_request(cunit *p, embi_js_value spec, embi_js_value type, bool unsupported)
{
    cmod *m = p->mod;
    for (uint32_t i = 0; i < m->reqs.len; i++) {
        if (m->reqs.data[i].spec == spec && m->reqs.data[i].type == type &&
            m->reqs.data[i].unsupported == unsupported) {
            return i;
        }
    }
    cmreq r = {spec, type, unsupported};
    embi_vec_push(&m->reqs, r);
    return (uint32_t)m->reqs.len - 1u;
}

/* A module specifier (a string literal) and its attributes: with { type: "json" } gives the type
 * (EMPTY when there is none). */
static bool mod_specifier(cunit *p, embi_js_value *spec, embi_js_value *type, bool *unsupported)
{
    *unsupported = false;
    if (!is(p, EMBI_JST_STRING)) {
        return unexpected(p);
    }
    *spec = mod_string_atom(p);
    adv(p);
    *type = EMBI_JS_EMPTY;
    if (!is_kw(p, EMBI_JSKW_with)) {
        return true;
    }
    adv(p);
    if (!expect(p, EMBI_JST_LBRACE)) {
        return false;
    }
    EMBI_VEC(embi_js_value) seen = {nullptr, 0, 0};
    embi_js_value type_key = catom(p, "type");
    while (!is(p, EMBI_JST_RBRACE)) {
        embi_js_value key;
        bool was_string;
        if (!mod_name(p, &key, &was_string)) {
            embi_vec_free(&seen);
            return false;
        }
        if (!expect(p, EMBI_JST_COLON)) {
            embi_vec_free(&seen);
            return false;
        }
        if (!is(p, EMBI_JST_STRING)) {
            embi_vec_free(&seen);
            return unexpected(p);
        }
        embi_js_value val = mod_string_atom(p);
        adv(p);
        for (uint32_t i = 0; i < seen.len; i++) {
            if (seen.data[i] == key) {
                embi_vec_free(&seen);
                return perr(p, "Import attribute has a duplicate key");
            }
        }
        embi_vec_push(&seen, key);
        if (key == type_key) {
            *type = val;
        } else {
            *unsupported = true; /* only "type" is supported: the load of the request fails */
        }
        if (!is(p, EMBI_JST_COMMA)) {
            break;
        }
        adv(p);
    }
    embi_vec_free(&seen);
    return expect(p, EMBI_JST_RBRACE);
}

static bool mod_add_export(cunit *p, uint8_t kind, embi_js_value name, embi_js_value local,
                           uint32_t req)
{
    cmod *m = p->mod;
    if (kind != CMEX_STAR) {
        for (uint32_t i = 0; i < m->exps.len; i++) {
            if (m->exps.data[i].kind != CMEX_STAR && m->exps.data[i].name == name) {
                return perr(p, "Duplicate export name");
            }
        }
    }
    cmexp e = {kind, name, local, req, -1};
    embi_vec_push(&m->exps, e);
    return true;
}

/* import ImportClause FromClause AttributeClause ;  (or import ModuleSpecifier ;) */
static bool parse_import_decl(cunit *p)
{
    cmod *m = p->mod;
    adv(p); /* import */
    if (is(p, EMBI_JST_STRING)) {
        embi_js_value spec, type;
        bool unsup;
        if (!mod_specifier(p, &spec, &type, &unsup)) {
            return false;
        }
        mod_request(p, spec, type, unsup);
        return consume_semi(p);
    }
    /* The imports of this declaration get their request once the specifier is read. */
    uint32_t first = (uint32_t)m->imps.len;
    bool need_clause = false;
    if (is_ident(p)) {
        embi_js_value local;
        if (!mod_binding_ident(p, &local)) {
            return false;
        }
        cmimp imp = {catom(p, "default"), local, UINT32_MAX};
        embi_vec_push(&m->imps, imp);
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
            need_clause = true;
        } else if (!tok_is_word(p, "from")) {
            return unexpected(p);
        }
    }
    if (is(p, EMBI_JST_MUL)) {
        adv(p);
        if (!tok_is_word(p, "as")) {
            return unexpected(p);
        }
        adv(p);
        embi_js_value local;
        if (!mod_binding_ident(p, &local)) {
            return false;
        }
        cmimp imp = {EMBI_JS_EMPTY, local, UINT32_MAX};
        embi_vec_push(&m->imps, imp);
    } else if (is(p, EMBI_JST_LBRACE)) {
        adv(p);
        while (!is(p, EMBI_JST_RBRACE)) {
            bool id_ok = is_ident(p);
            embi_js_value imp_name;
            bool was_string;
            if (!mod_name(p, &imp_name, &was_string)) {
                return false;
            }
            embi_js_value local = imp_name;
            if (tok_is_word(p, "as")) {
                adv(p);
                if (!mod_binding_ident(p, &local)) {
                    return false;
                }
            } else {
                if (was_string || !id_ok) {
                    return perr(p, "Unexpected token in import specifier");
                }
                if (!check_strict_name(p, local)) {
                    return false;
                }
            }
            cmimp imp = {imp_name, local, UINT32_MAX};
            embi_vec_push(&m->imps, imp);
            if (!is(p, EMBI_JST_COMMA)) {
                break;
            }
            adv(p);
        }
        if (!expect(p, EMBI_JST_RBRACE)) {
            return false;
        }
    } else if (m->imps.len == first || need_clause) {
        return unexpected(p);
    }
    if (!tok_is_word(p, "from")) {
        return unexpected(p);
    }
    adv(p);
    embi_js_value spec, type;
    bool unsup;
    if (!mod_specifier(p, &spec, &type, &unsup)) {
        return false;
    }
    uint32_t req = mod_request(p, spec, type, unsup);
    for (uint32_t i = first; i < m->imps.len; i++) {
        m->imps.data[i].req = req;
        embi_js_value local = m->imps.data[i].local;
        if (embi_jsc_find_in_scope(p->top->root, local)) {
            return perr(p, "Identifier has already been declared");
        }
        cbind *b = embi_jsc_new_bind(p, p->top, p->top->root, local, BK_IMPORT);
        b->import_token = import_token_new(p, p->holder, i);
        embi_jsc_add_const(p, b->import_token);
    }
    return consume_semi(p);
}

/* export default HoistableDeclaration | ClassDeclaration | AssignmentExpression ;
 * An anonymous function or class is bound to *default* and named "default". */
static bool parse_export_default(cunit *p)
{
    embi_js_value dflt = catom(p, "default");
    embi_js_value star_default = catom(p, "*default*");
    adv(p); /* default */
    bool async = is_kw(p, EMBI_JSKW_async) && async_function_ahead(p);
    if (is_kw(p, EMBI_JSKW_function) || async) {
        uint32_t fn_start = p->tok.start;
        if (async) {
            adv(p);
        }
        adv(p); /* function */
        bool gen = false;
        if (is(p, EMBI_JST_MUL)) {
            gen = true;
            adv(p);
        }
        embi_js_value bind_name = star_default;
        embi_js_value fn_name = dflt;
        if (is_ident(p)) {
            fn_name = name_atom(p);
            bind_name = fn_name;
            if (!check_strict_name(p, fn_name)) {
                return false;
            }
            adv(p);
        }
        if (!check_var_conflict(p, bind_name)) {
            return false;
        }
        cbind *b = declare_var(p, bind_name, BK_FUNC);
        if (!b) {
            return false;
        }
        p->next_gen = gen;
        p->next_async = async;
        cfunc *f;
        if (!parse_function_common(p, fn_name, EMBI_JS_EMPTY, fn_start, &f)) {
            return false;
        }
        cdecl d = {f, b, bind_name};
        CF_PUSH(p->fn, decls, d);
        return mod_add_export(p, CMEX_LOCAL, dflt, bind_name, 0);
    }
    bool need_semi = true;
    if (is_kw(p, EMBI_JSKW_class)) {
        embi_jstok nt;
        peek(p, &nt);
        bool anonymous = nt.kind == EMBI_JST_LBRACE ||
                         (nt.kind == EMBI_JST_NAME && nt.kw == EMBI_JSKW_extends);
        if (!anonymous) {
            cvalvec names = {nullptr, 0, 0};
            cvalvec *saved = p->collect;
            p->collect = &names;
            expr unused;
            bool ok = parse_class(p, &unused, true);
            p->collect = saved;
            bool added = ok && names.len == 1 && mod_add_export(p, CMEX_LOCAL, dflt, names.data[0], 0);
            embi_vec_free(&names);
            return ok && added;
        }
        expr e;
        p->hint = dflt;
        if (!parse_class(p, &e, false)) {
            return false;
        }
        to_value(p, &e);
        need_semi = false; /* an anonymous class declaration ends at its closing brace */
    } else {
        expr e;
        /* NamedEvaluation: an anonymous function, class or arrow gets the name "default". */
        p->hint = dflt;
        bool ok = parse_assign(p, false, &e);
        p->hint = EMBI_JS_EMPTY;
        if (!ok) {
            return false;
        }
        to_value(p, &e);
    }
    bool ok;
    cbind *b = declare_lexical(p, star_default, BK_CONST, &ok);
    if (!ok) {
        return false;
    }
    embi_jsc_emit_sym(p, SYM_KIND_INIT, star_default, p->fn->cur, b);
    if (need_semi && !consume_semi(p)) {
        return false;
    }
    return mod_add_export(p, CMEX_LOCAL, dflt, star_default, 0);
}

/* export * from, export * as ns from, export { ... } [from], export VariableStatement,
 * export Declaration, export default. */
static bool parse_export_decl(cunit *p)
{
    adv(p); /* export */
    if (is(p, EMBI_JST_MUL)) {
        adv(p);
        embi_js_value exported = EMBI_JS_EMPTY;
        bool has_name = false;
        if (tok_is_word(p, "as")) {
            adv(p);
            bool was_string;
            if (!mod_name(p, &exported, &was_string)) {
                return false;
            }
            has_name = true;
        }
        if (!tok_is_word(p, "from")) {
            return unexpected(p);
        }
        adv(p);
        embi_js_value spec, type;
        bool unsup;
        if (!mod_specifier(p, &spec, &type, &unsup)) {
            return false;
        }
        uint32_t req = mod_request(p, spec, type, unsup);
        if (has_name) {
            if (!mod_add_export(p, CMEX_INDIRECT, exported, EMBI_JS_EMPTY, req)) {
                return false;
            }
        } else if (!mod_add_export(p, CMEX_STAR, EMBI_JS_EMPTY, EMBI_JS_EMPTY, req)) {
            return false;
        }
        return consume_semi(p);
    }
    if (is_kw(p, EMBI_JSKW_default)) {
        return parse_export_default(p);
    }
    if (is(p, EMBI_JST_LBRACE)) {
        adv(p);
        EMBI_VEC(mod_spec) items = {nullptr, 0, 0};
        while (!is(p, EMBI_JST_RBRACE)) {
            mod_spec it;
            it.local_ident = is_ident(p);
            if (!mod_name(p, &it.local, &it.local_string)) {
                embi_vec_free(&items);
                return false;
            }
            it.exported = it.local;
            if (tok_is_word(p, "as")) {
                adv(p);
                bool exp_string;
                if (!mod_name(p, &it.exported, &exp_string)) {
                    embi_vec_free(&items);
                    return false;
                }
            }
            embi_vec_push(&items, it);
            if (!is(p, EMBI_JST_COMMA)) {
                break;
            }
            adv(p);
        }
        if (!expect(p, EMBI_JST_RBRACE)) {
            embi_vec_free(&items);
            return false;
        }
        if (tok_is_word(p, "from")) {
            adv(p);
            embi_js_value spec, type;
            bool unsup;
            if (!mod_specifier(p, &spec, &type, &unsup)) {
                embi_vec_free(&items);
                return false;
            }
            uint32_t req = mod_request(p, spec, type, unsup);
            for (uint32_t i = 0; i < items.len; i++) {
                if (!mod_add_export(p, CMEX_INDIRECT, items.data[i].exported, items.data[i].local,
                                    req)) {
                    embi_vec_free(&items);
                    return false;
                }
            }
        } else {
            for (uint32_t i = 0; i < items.len; i++) {
                if (items.data[i].local_string || !items.data[i].local_ident) {
                    embi_vec_free(&items);
                    return perr(p, "Unexpected reserved word in export specifier");
                }
                if (!mod_add_export(p, CMEX_LOCAL, items.data[i].exported, items.data[i].local, 0)) {
                    embi_vec_free(&items);
                    return false;
                }
            }
        }
        embi_vec_free(&items);
        return consume_semi(p);
    }
    /* A declaration: the names it binds are exported under their own names. The collection is
     * limited to the module's top function (declare_var and declare_lexical check it). */
    cvalvec names = {nullptr, 0, 0};
    cvalvec *saved = p->collect;
    p->collect = &names;
    bool ok;
    if (is_kw(p, EMBI_JSKW_var)) {
        adv(p);
        embi_js_value unused = EMBI_JS_EMPTY;
        ok = parse_var_decls(p, false, false, &unused) && consume_semi(p);
    } else if (is_kw(p, EMBI_JSKW_let) || is_kw(p, EMBI_JSKW_const)) {
        bool is_const = is_kw(p, EMBI_JSKW_const);
        adv(p);
        embi_js_value unused = EMBI_JS_EMPTY;
        cbind *unused_b = nullptr;
        ok = parse_lexical_decls(p, is_const, false, false, &unused, &unused_b) && consume_semi(p);
    } else if (is_kw(p, EMBI_JSKW_function) ||
               (is_kw(p, EMBI_JSKW_async) && async_function_ahead(p))) {
        ok = parse_function_decl_kw(p, is_kw(p, EMBI_JSKW_async));
    } else if (is_kw(p, EMBI_JSKW_class)) {
        expr unused;
        ok = parse_class(p, &unused, true);
    } else {
        p->collect = saved;
        embi_vec_free(&names);
        return unexpected(p);
    }
    p->collect = saved;
    bool added = ok;
    for (uint32_t i = 0; ok && i < names.len; i++) {
        added = mod_add_export(p, CMEX_LOCAL, names.data[i], names.data[i], 0);
        if (!added) {
            break;
        }
    }
    embi_vec_free(&names);
    return added;
}

/* A module item: an import or export declaration, or a statement. */
static bool module_item(cunit *p)
{
    if (is_kw(p, EMBI_JSKW_import) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind != EMBI_JST_LPAREN && nt.kind != EMBI_JST_DOT) {
            return parse_import_decl(p);
        }
    }
    if (is_kw(p, EMBI_JSKW_export) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
        return parse_export_decl(p);
    }
    return parse_statement(p);
}

/* import(specifier [, options]) and import.meta, in an expression. */
static bool parse_import_expr(cunit *p, expr *e)
{
    embi_jstok nt;
    peek(p, &nt);
    if (nt.kind == EMBI_JST_DOT) {
        if (!p->mod || p->eval_mode) {
            return perr(p, "Cannot use 'import.meta' outside a module");
        }
        adv(p); /* import */
        adv(p); /* . */
        if (!(is(p, EMBI_JST_NAME) && tok_is_word(p, "meta"))) {
            return unexpected(p);
        }
        adv(p);
        OPC32(IMPORT_META, const_str(p, p->holder));
        set_value(e);
        return true;
    }
    if (nt.kind != EMBI_JST_LPAREN) {
        return unexpected(p);
    }
    adv(p); /* import */
    adv(p); /* ( */
    expr spec;
    if (!parse_assign(p, false, &spec)) {
        return false;
    }
    to_value(p, &spec);
    if (is(p, EMBI_JST_COMMA)) {
        adv(p);
        if (!is(p, EMBI_JST_RPAREN)) {
            expr opts;
            if (!parse_assign(p, false, &opts)) {
                return false;
            }
            to_value(p, &opts);
            if (is(p, EMBI_JST_COMMA)) {
                adv(p);
            }
        } else {
            OPC(PUSH_UNDEF);
        }
    } else {
        OPC(PUSH_UNDEF);
    }
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    /* The referrer is the enclosing module, also from direct eval code inside it (p->eval_module). */
    embi_js_value ref = p->eval_mode ? p->eval_module : (p->mod ? p->holder : EMBI_JS_EMPTY);
    uint32_t k = (ref != EMBI_JS_EMPTY && embi_js_is_object(ref)) ? const_str(p, ref)
                                                                   : const_str(p, EMBI_JS_UNDEFINED);
    embi_jsc_emit_op32(p, EMBI_JSOP_DYN_IMPORT, k);
    set_value(e);
    return true;
}

/* After the module body: an export of a local name must name a declared binding, and an export of
 * an import is an indirect export of the import's module. */
static bool mod_finish_exports(cunit *p)
{
    cmod *m = p->mod;
    for (uint32_t i = 0; i < m->exps.len; i++) {
        cmexp *e = &m->exps.data[i];
        if (e->kind != CMEX_LOCAL) {
            continue;
        }
        cbind *b = embi_jsc_find_in_scope(p->top->root, e->local);
        if (!b) {
            return perr(p, "Export name refers to a binding that is not declared in the module");
        }
        if (b->kind == BK_IMPORT) {
            for (uint32_t j = 0; j < m->imps.len; j++) {
                if (m->imps.data[j].local == e->local) {
                    e->kind = CMEX_INDIRECT;
                    e->req = m->imps.data[j].req;
                    e->local = m->imps.data[j].name;
                    break;
                }
            }
        }
    }
    return true;
}

/* Parses a module body into p (p->mod is the module's metadata, p->holder its holder). */
bool embi_jsc_parse_module(cunit *p)
{
    cfunc *top = embi_jsc_new_func(p, nullptr, nullptr);
    top->is_module = true;
    top->strict = true;
    top->is_async = true; /* await is a keyword here; the flag is reset to uses_await below */
    p->top = top;
    p->fn = top;
    p->ctl = nullptr;
    OPC32(JMP, 0);
    OPC(NOP); /* pc 5: the body start (ASYNC_START when the module uses await) */
    adv(p);
    while (!is(p, EMBI_JST_EOF)) {
        if (!module_item(p)) {
            return false;
        }
        if (p->late_err) {
            return perr(p, "Identifier has already been declared");
        }
    }
    OPC(PUSH_UNDEF);
    OPC(RETURN);
    if (!mod_finish_exports(p)) {
        return false;
    }
    p->mod->has_tla = top->uses_await;
    top->is_async = top->uses_await;
    if (top->uses_await) {
        top->code.data[5] = EMBI_JSOP_ASYNC_START;
    }
    return finish_all(p);
}
