/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/flet.h"
#include "util/scoped_map.h"
#include "kernel/environment.h"
#include "kernel/normalizer.h"
#include "kernel/builtin.h"
#include "kernel/kernel_exception.h"
#include "kernel/instantiate.h"
#include "kernel/free_vars.h"
#include "kernel/metavar.h"
#include "library/reduce.h"
#include "library/light_checker.h"

namespace lean {
class light_checker::imp {
    typedef scoped_map<expr, expr, expr_hash, expr_eqp> cache;

    environment               m_env;
    context                   m_ctx;
    substitution *            m_subst;
    unsigned                  m_subst_timestamp;
    unification_constraints * m_uc;
    normalizer                m_normalizer;
    cache                     m_cache;
    volatile bool             m_interrupted;


    level infer_universe(expr const & t, context const & ctx) {
        expr u = m_normalizer(infer_type(t, ctx), ctx);
        if (is_type(u))
            return ty_level(u);
        if (u == Bool)
            return level();
        throw type_expected_exception(m_env, ctx, t);
    }

    expr get_range(expr t, expr const & e, context const & ctx) {
        unsigned num = num_args(e) - 1;
        while (num > 0) {
            --num;
            if (is_pi(t)) {
                t = abst_body(t);
            } else {
                t = m_normalizer(t, ctx);
                if (is_pi(t)) {
                    t = abst_body(t);
                } else {
                    throw function_expected_exception(m_env, ctx, e);
                }
            }
        }
        if (closed(t))
            return t;
        else
            return instantiate(t, num_args(e)-1, &arg(e, 1));
    }

    void set_subst(substitution * subst) {
        if (m_subst == subst) {
            // Check whether m_subst has been updated since the last time the normalizer has been invoked
            if (m_subst && m_subst->get_timestamp() > m_subst_timestamp) {
                m_subst_timestamp = m_subst->get_timestamp();
                m_cache.clear();
            }
        } else {
            m_subst = subst;
            m_cache.clear();
            m_subst_timestamp = m_subst ? m_subst->get_timestamp() : 0;
        }
    }

    expr infer_type(expr const & e, context const & ctx) {
        // cheap cases, we do not cache results
        switch (e.kind()) {
        case expr_kind::MetaVar: {
            expr r = metavar_type(e);
            if (!r)
                throw kernel_exception(m_env, "metavariable does not have a type associated with it");
            return r;
        }
        case expr_kind::Constant: {
            object const & obj = m_env.get_object(const_name(e));
            if (obj.has_type())
                return obj.get_type();
            else
                throw exception("type incorrect expression");
            break;
        }
        case expr_kind::Var: {
            context_entry const & ce = lookup(ctx, var_idx(e));
            if (ce.get_domain())
                return ce.get_domain();
            // Remark: the case where ce.get_domain() is not
            // available is not considered cheap.
            break;
        }
        case expr_kind::Eq:
            return mk_bool_type();
        case expr_kind::Value:
            return to_value(e).get_type();
        case expr_kind::Type:
            return mk_type(ty_level(e) + 1);
        case expr_kind::App: case expr_kind::Lambda:
        case expr_kind::Pi:  case expr_kind::Let:
            break; // expensive cases
        }

        check_interrupted(m_interrupted);
        bool shared = false;
        if (is_shared(e)) {
            shared = true;
            auto it = m_cache.find(e);
            if (it != m_cache.end())
                return it->second;
        }

        expr r;
        switch (e.kind()) {
        case expr_kind::Constant: case expr_kind::Eq:
        case expr_kind::Value:    case expr_kind::Type:
        case expr_kind::MetaVar:
            lean_unreachable();
        case expr_kind::Var: {
            auto p = lookup_ext(ctx, var_idx(e));
            context_entry const & ce = p.first;
            context const & ce_ctx   = p.second;
            lean_assert(!ce.get_domain());
            r = lift_free_vars(infer_type(ce.get_body(), ce_ctx), ctx.size() - ce_ctx.size());
            break;
        }
        case expr_kind::App: {
            expr const & f = arg(e, 0);
            expr f_t = infer_type(f, ctx);
            r = get_range(f_t, e, ctx);
            break;
        }
        case expr_kind::Lambda: {
            cache::mk_scope sc(m_cache);
            r = mk_pi(abst_name(e), abst_domain(e), infer_type(abst_body(e), extend(ctx, abst_name(e), abst_domain(e))));
            break;
        }
        case expr_kind::Pi: {
            level l1 = infer_universe(abst_domain(e), ctx);
            level l2;
            {
                cache::mk_scope sc(m_cache);
                l2 = infer_universe(abst_body(e), extend(ctx, abst_name(e), abst_domain(e)));
            }
            r = mk_type(max(l1, l2));
            break;
        }
        case expr_kind::Let: {
            cache::mk_scope sc(m_cache);
            r = infer_type(let_body(e), extend(ctx, let_name(e), let_type(e), let_value(e)));
            break;
        }}

        if (shared) {
            m_cache.insert(e, r);
        }
        return r;
    }

    void set_ctx(context const & ctx) {
        if (!is_eqp(m_ctx, ctx)) {
            clear();
            m_ctx = ctx;
        }
    }

public:
    imp(environment const & env):
        m_env(env),
        m_normalizer(env) {
        m_interrupted     = false;
        m_subst           = nullptr;
        m_subst_timestamp = 0;
        m_uc              = nullptr;
    }

    expr operator()(expr const & e, context const & ctx, substitution * subst, unification_constraints * uc) {
        set_ctx(ctx);
        set_subst(subst);
        flet<unification_constraints*> set(m_uc, uc);
        return infer_type(e, ctx);
    }

    void set_interrupt(bool flag) {
        m_interrupted = flag;
        m_normalizer.set_interrupt(flag);
    }

    void clear() {
        m_cache.clear();
        m_normalizer.clear();
        m_ctx            = context();
        m_subst = nullptr;
        m_subst_timestamp = 0;
    }
};
light_checker::light_checker(environment const & env):m_ptr(new imp(env)) {}
light_checker::~light_checker() {}
expr light_checker::operator()(expr const & e, context const & ctx, substitution * subst, unification_constraints * uc) {
    return m_ptr->operator()(e, ctx, subst, uc);
}
void light_checker::clear() { m_ptr->clear(); }
void light_checker::set_interrupt(bool flag) { m_ptr->set_interrupt(flag); }
}
