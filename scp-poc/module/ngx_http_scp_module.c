/*
 * ngx_http_scp_module - a minimal 3GPP Release-16 Service Communication
 * Proxy (SCP) front-end for the 5G Core Service Based Interface (SBI).
 *
 * It layers SBI awareness on top of nginx's native HTTP/2 upstream
 * (proxy_http_version 2):
 *
 *   - Parses and validates the 3gpp-Sbi-Target-apiRoot header and exposes
 *     $scp_backend / $scp_target_authority / $scp_target_scheme so that a
 *     variabilised proxy_pass can route Model-D (indirect) requests to the
 *     correct NF-set upstream{} block (load balancing + reselection).
 *   - Optionally enforces an OAuth2 bearer token (TS 33.501).
 *   - Returns spec-shaped application/problem+json ProblemDetails on error
 *     (TS 29.571).
 *   - Logs the routing decision for observability.
 *
 * This is a hackathon proof-of-concept, not a production SCP.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_SCP_TARGET_HEADER      "3gpp-Sbi-Target-apiRoot"
#define NGX_HTTP_SCP_TARGET_HEADER_LEN  (sizeof(NGX_HTTP_SCP_TARGET_HEADER) - 1)


typedef struct {
    ngx_flag_t   enable;
    ngx_flag_t   require_oauth;
    ngx_flag_t   require_target;
} ngx_http_scp_loc_conf_t;


typedef struct {
    unsigned     parsed:1;
    unsigned     valid:1;
    ngx_str_t    scheme;
    ngx_str_t    authority;   /* host[:port]           */
    ngx_str_t    host;        /* host only -> backend  */
} ngx_http_scp_ctx_t;


static ngx_int_t ngx_http_scp_handler(ngx_http_request_t *r);
static ngx_http_scp_ctx_t *ngx_http_scp_get_ctx(ngx_http_request_t *r);
static void ngx_http_scp_parse(ngx_http_request_t *r, ngx_http_scp_ctx_t *ctx);
static ngx_table_elt_t *ngx_http_scp_find_header(ngx_http_request_t *r,
    u_char *name, size_t len);
static ngx_uint_t ngx_http_scp_has_bearer(ngx_http_request_t *r);
static ngx_int_t ngx_http_scp_problem(ngx_http_request_t *r, ngx_uint_t status,
    const char *title, const char *cause, const char *detail);
static ngx_int_t ngx_http_scp_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static ngx_int_t ngx_http_scp_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_scp_init(ngx_conf_t *cf);
static void *ngx_http_scp_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_scp_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);


enum {
    NGX_HTTP_SCP_VAR_BACKEND = 0,
    NGX_HTTP_SCP_VAR_AUTHORITY,
    NGX_HTTP_SCP_VAR_SCHEME,
    NGX_HTTP_SCP_VAR_HAS_TOKEN
};


static ngx_command_t  ngx_http_scp_commands[] = {

    { ngx_string("scp"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_scp_loc_conf_t, enable),
      NULL },

    { ngx_string("scp_require_oauth"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_scp_loc_conf_t, require_oauth),
      NULL },

    { ngx_string("scp_require_target"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_scp_loc_conf_t, require_target),
      NULL },

      ngx_null_command
};


static ngx_http_variable_t  ngx_http_scp_vars[] = {

    { ngx_string("scp_backend"), NULL, ngx_http_scp_variable,
      NGX_HTTP_SCP_VAR_BACKEND, 0, 0 },

    { ngx_string("scp_target_authority"), NULL, ngx_http_scp_variable,
      NGX_HTTP_SCP_VAR_AUTHORITY, 0, 0 },

    { ngx_string("scp_target_scheme"), NULL, ngx_http_scp_variable,
      NGX_HTTP_SCP_VAR_SCHEME, 0, 0 },

    { ngx_string("scp_has_token"), NULL, ngx_http_scp_variable,
      NGX_HTTP_SCP_VAR_HAS_TOKEN, 0, 0 },

      ngx_http_null_variable
};


static ngx_http_module_t  ngx_http_scp_module_ctx = {
    ngx_http_scp_add_variables,            /* preconfiguration */
    ngx_http_scp_init,                     /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_scp_create_loc_conf,          /* create location configuration */
    ngx_http_scp_merge_loc_conf            /* merge location configuration */
};


ngx_module_t  ngx_http_scp_module = {
    NGX_MODULE_V1,
    &ngx_http_scp_module_ctx,              /* module context */
    ngx_http_scp_commands,                 /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_scp_handler(ngx_http_request_t *r)
{
    ngx_http_scp_ctx_t       *ctx;
    ngx_http_scp_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_scp_module);

    if (!slcf->enable) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_scp_get_ctx(r);
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (slcf->require_target && !ctx->valid) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "scp: missing or malformed %s header",
                      NGX_HTTP_SCP_TARGET_HEADER);

        return ngx_http_scp_problem(r, NGX_HTTP_BAD_REQUEST, "Bad Request",
                      "MANDATORY_IE_MISSING",
                      "3gpp-Sbi-Target-apiRoot header is missing or malformed");
    }

    if (slcf->require_oauth && !ngx_http_scp_has_bearer(r)) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "scp: missing OAuth2 bearer token");

        return ngx_http_scp_problem(r, NGX_HTTP_UNAUTHORIZED, "Unauthorized",
                      "UNAUTHORIZED",
                      "OAuth2 access token required for SBI access (TS 33.501)");
    }

    if (ctx->valid) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "scp: route apiRoot=%V://%V backend=%V uri=%V",
                      &ctx->scheme, &ctx->authority, &ctx->host, &r->uri);
    }

    return NGX_DECLINED;
}


static ngx_http_scp_ctx_t *
ngx_http_scp_get_ctx(ngx_http_request_t *r)
{
    ngx_http_scp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_scp_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_scp_ctx_t));
        if (ctx == NULL) {
            return NULL;
        }
        ngx_http_set_ctx(r, ctx, ngx_http_scp_module);
    }

    if (!ctx->parsed) {
        ngx_http_scp_parse(r, ctx);
    }

    return ctx;
}


static void
ngx_http_scp_parse(ngx_http_request_t *r, ngx_http_scp_ctx_t *ctx)
{
    u_char           *p, *last, *authority_start, *authority_end, *colon;
    ngx_str_t         value;
    ngx_table_elt_t  *h;

    ctx->parsed = 1;
    ctx->valid = 0;

    h = ngx_http_scp_find_header(r, (u_char *) NGX_HTTP_SCP_TARGET_HEADER,
                                 NGX_HTTP_SCP_TARGET_HEADER_LEN);
    if (h == NULL || h->value.len == 0) {
        return;
    }

    value = h->value;
    last = value.data + value.len;

    /* locate the scheme separator "://" */

    authority_start = value.data;

    for (p = value.data; p + 3 <= last; p++) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
            ctx->scheme.data = value.data;
            ctx->scheme.len = p - value.data;
            authority_start = p + 3;
            break;
        }
    }

    if (ctx->scheme.len == 0) {
        ngx_str_set(&ctx->scheme, "http");
    }

    /* authority ends at the first '/' (path) or end of value */

    authority_end = authority_start;
    while (authority_end < last && *authority_end != '/') {
        authority_end++;
    }

    if (authority_start >= authority_end) {
        return;
    }

    ctx->authority.data = authority_start;
    ctx->authority.len = authority_end - authority_start;

    /* host is the authority up to an optional ':' port */

    colon = authority_start;
    while (colon < authority_end && *colon != ':') {
        colon++;
    }

    ctx->host.data = authority_start;
    ctx->host.len = colon - authority_start;

    if (ctx->host.len > 0) {
        ctx->valid = 1;
    }
}


static ngx_table_elt_t *
ngx_http_scp_find_header(ngx_http_request_t *r, u_char *name, size_t len)
{
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == len
            && ngx_strncasecmp(h[i].key.data, name, len) == 0)
        {
            return &h[i];
        }
    }

    return NULL;
}


static ngx_uint_t
ngx_http_scp_has_bearer(ngx_http_request_t *r)
{
    ngx_table_elt_t  *a;

    a = r->headers_in.authorization;

    if (a == NULL || a->value.len < 7) {
        return 0;
    }

    if (ngx_strncasecmp(a->value.data, (u_char *) "Bearer ", 7) == 0) {
        return 1;
    }

    return 0;
}


static ngx_int_t
ngx_http_scp_problem(ngx_http_request_t *r, ngx_uint_t status,
    const char *title, const char *cause, const char *detail)
{
    u_char            *body, *end;
    size_t             len;
    ngx_int_t          rc;
    ngx_buf_t         *b;
    ngx_chain_t        out;
    static ngx_str_t   ct = ngx_string("application/problem+json");

    len = ngx_strlen(title) + ngx_strlen(cause) + ngx_strlen(detail) + 96;

    body = ngx_pnalloc(r->pool, len);
    if (body == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
    }

    end = ngx_sprintf(body,
                      "{\"title\":\"%s\",\"status\":%ui,"
                      "\"cause\":\"%s\",\"detail\":\"%s\"}",
                      title, status, cause, detail);
    len = end - body;

    r->headers_out.status = status;
    r->headers_out.content_length_n = len;
    r->headers_out.content_type = ct;
    r->headers_out.content_type_len = ct.len;
    r->headers_out.content_type_lowcase = NULL;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        ngx_http_finalize_request(r, rc);
        return NGX_DONE;
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, rc);
        return NGX_DONE;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
    }

    ngx_memcpy(b->pos, body, len);
    b->last = b->pos + len;
    b->last_buf = 1;
    b->last_in_chain = 1;

    out.buf = b;
    out.next = NULL;

    rc = ngx_http_output_filter(r, &out);
    ngx_http_finalize_request(r, rc);

    return NGX_DONE;
}


static ngx_int_t
ngx_http_scp_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_str_t            s;
    ngx_http_scp_ctx_t  *ctx;

    if (data == NGX_HTTP_SCP_VAR_HAS_TOKEN) {
        v->valid = 1;
        v->no_cacheable = 1;
        v->not_found = 0;
        v->len = 1;
        v->data = (u_char *) (ngx_http_scp_has_bearer(r) ? "1" : "0");
        return NGX_OK;
    }

    ctx = ngx_http_scp_get_ctx(r);
    if (ctx == NULL || !ctx->valid) {
        v->not_found = 1;
        return NGX_OK;
    }

    switch (data) {
    case NGX_HTTP_SCP_VAR_BACKEND:
        s = ctx->host;
        break;
    case NGX_HTTP_SCP_VAR_AUTHORITY:
        s = ctx->authority;
        break;
    case NGX_HTTP_SCP_VAR_SCHEME:
        s = ctx->scheme;
        break;
    default:
        v->not_found = 1;
        return NGX_OK;
    }

    if (s.len == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->len = s.len;
    v->data = s.data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_scp_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_scp_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_scp_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_scp_handler;

    return NGX_OK;
}


static void *
ngx_http_scp_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_scp_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_scp_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enable = NGX_CONF_UNSET;
    conf->require_oauth = NGX_CONF_UNSET;
    conf->require_target = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_scp_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_scp_loc_conf_t  *prev = parent;
    ngx_http_scp_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->require_oauth, prev->require_oauth, 0);
    ngx_conf_merge_value(conf->require_target, prev->require_target, 1);

    return NGX_CONF_OK;
}
