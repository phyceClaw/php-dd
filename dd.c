/*
 * dd — a native, compiled PHP language feature equivalent to Laravel's dd()/dump().
 *
 * Provides two global functions implemented in C against the Zend engine:
 *
 *   dd(...$vars)   — dump every argument, then halt the process (exit 1).
 *   dump(...$vars) — dump every argument and return them (1 arg => that arg,
 *                    otherwise an array of the args), like Symfony's dump().
 *
 * Output auto-detects the SAPI, mirroring Symfony VarDumper (what Laravel uses):
 *   - web SAPI            -> styled HTML <pre class=sf-dump> block
 *   - CLI on a TTY        -> ANSI 256-colour output
 *   - CLI piped/NO_COLOR  -> plain text
 *
 * The wire format (array:N [...], object {#id ...}, +/#/- visibility markers,
 * the colour palette) matches Symfony VarDumper's default theme.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_smart_str.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "SAPI.h"
#include "php_dd.h"

ZEND_DECLARE_MODULE_GLOBALS(dd)

/* ------------------------------------------------------------------ *
 * Output modes
 * ------------------------------------------------------------------ */

#define DD_MODE_PLAIN 0
#define DD_MODE_ANSI  1
#define DD_MODE_HTML  2

/* ------------------------------------------------------------------ *
 * Styling
 * ------------------------------------------------------------------ */

typedef enum {
	S_DEFAULT = 0,
	S_NUM,
	S_STR,
	S_NOTE,
	S_CONST,
	S_KEY,
	S_INDEX,
	S_REF,
	S_META
} dd_style;

/* Symfony VarDumper CliDumper default 256-colour palette. */
static const char *const ansi_codes[] = {
	"38;5;208",    /* default */
	"1;38;5;38",   /* num     */
	"1;38;5;113",  /* str     */
	"38;5;38",     /* note    */
	"1;38;5;208",  /* const   */
	"38;5;113",    /* key     */
	"38;5;38",     /* index   */
	"38;5;247",    /* ref     */
	"38;5;170"     /* meta    */
};

/* Symfony VarDumper HtmlDumper class names. */
static const char *const html_classes[] = {
	"sf-dump-default",
	"sf-dump-num",
	"sf-dump-str",
	"sf-dump-note",
	"sf-dump-const",
	"sf-dump-key",
	"sf-dump-index",
	"sf-dump-ref",
	"sf-dump-meta"
};

static void s_open(smart_str *b, int mode, dd_style s)
{
	if (mode == DD_MODE_HTML) {
		smart_str_appends(b, "<span class=");
		smart_str_appends(b, html_classes[s]);
		smart_str_appendc(b, '>');
	} else if (mode == DD_MODE_ANSI) {
		smart_str_appends(b, "\033[");
		smart_str_appends(b, ansi_codes[s]);
		smart_str_appendc(b, 'm');
	}
}

static void s_close(smart_str *b, int mode, dd_style s)
{
	(void) s;
	if (mode == DD_MODE_HTML) {
		smart_str_appends(b, "</span>");
	} else if (mode == DD_MODE_ANSI) {
		smart_str_appends(b, "\033[m");
	}
}

/* Append text, HTML-escaping when in HTML mode. */
static void app_text(smart_str *b, int mode, const char *s, size_t len)
{
	if (mode != DD_MODE_HTML) {
		smart_str_appendl(b, s, len);
		return;
	}
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		switch (c) {
			case '&': smart_str_appends(b, "&amp;");  break;
			case '<': smart_str_appends(b, "&lt;");   break;
			case '>': smart_str_appends(b, "&gt;");   break;
			case '"': smart_str_appends(b, "&quot;"); break;
			default:  smart_str_appendc(b, c);
		}
	}
}

static void s_text(smart_str *b, int mode, dd_style s, const char *txt, size_t len)
{
	s_open(b, mode, s);
	app_text(b, mode, txt, len);
	s_close(b, mode, s);
}

static void indent(smart_str *b, int depth)
{
	for (int i = 0; i < depth * 2; i++) {
		smart_str_appendc(b, ' ');
	}
}

/* Shortest round-tripping decimal, matching serialize_precision=-1, then
 * guarantee it reads as a float (append ".0" when it looks integral). */
static void format_double(char *buf, size_t sz, double d)
{
	if (isnan(d)) { strcpy(buf, "NAN"); return; }
	if (isinf(d)) { strcpy(buf, d < 0 ? "-INF" : "INF"); return; }

	for (int prec = 1; prec <= 17; prec++) {
		snprintf(buf, sz, "%.*g", prec, d);
		if (strtod(buf, NULL) == d) {
			break;
		}
	}
	if (!strpbrk(buf, ".eEnN")) {
		size_t l = strlen(buf);
		if (l + 2 < sz) {
			buf[l] = '.';
			buf[l + 1] = '0';
			buf[l + 2] = '\0';
		}
	}
}

/* ------------------------------------------------------------------ *
 * Recursive dumper
 * ------------------------------------------------------------------ */

static void dd_dump(smart_str *b, zval *z, int depth, int mode);

static void dump_string(smart_str *b, int mode, zend_string *s)
{
	s_open(b, mode, S_STR);
	smart_str_appendc(b, '"');
	app_text(b, mode, ZSTR_VAL(s), ZSTR_LEN(s));
	smart_str_appendc(b, '"');
	s_close(b, mode, S_STR);
}

/* Emit the opening of a collapsible compound node in HTML mode:
 *   <a toggle><arrow/><note>HEADER</note></a> OPEN<extra><samp ...>\n
 * `extra` is emitted after the opening bracket (used for an object's #id). */
static void html_open_compound(smart_str *b, int depth, dd_style header_style,
	const char *header, int header_len, char open_bracket,
	const char *extra, int extra_len)
{
	int collapsed = depth >= 1;
	smart_str_appends(b, "<a class=sf-dump-toggle href=#><span class=sf-dump-arrow>");
	smart_str_appends(b, collapsed ? "\xe2\x96\xb6" : "\xe2\x96\xbc"); /* > / v */
	smart_str_appends(b, "</span>");
	s_text(b, DD_MODE_HTML, header_style, header, header_len);
	smart_str_appends(b, "</a> ");
	smart_str_appendc(b, open_bracket);
	if (extra_len) {
		smart_str_appendl(b, extra, extra_len);
	}
	smart_str_appends(b, "<samp class=\"sf-dump-children");
	if (collapsed) {
		smart_str_appends(b, " sf-dump-collapsed");
	}
	smart_str_appends(b, "\" data-depth=");
	smart_str_append_long(b, (zend_long)(depth + 1));
	smart_str_appendc(b, '>');
	smart_str_appendc(b, '\n');
}

/* Emit the closing of a collapsible compound node in HTML mode:
 *   </samp><ellipsis>CLOSE  (ellipsis only shows while collapsed) */
static void html_close_compound(smart_str *b, uint32_t count, char close_bracket)
{
	smart_str_appends(b, "</samp><span class=sf-dump-ellip> \xe2\x80\xa6"); /* ... */
	smart_str_append_long(b, (zend_long) count);
	smart_str_appends(b, " </span>");
	smart_str_appendc(b, close_bracket);
}

static void dump_array(smart_str *b, zval *z, int depth, int mode)
{
	zend_array *ht = Z_ARRVAL_P(z);
	uint32_t count = zend_hash_num_elements(ht);

	if (GC_IS_RECURSIVE(ht)) {
		s_text(b, mode, S_REF, "array *RECURSION*", sizeof("array *RECURSION*") - 1);
		return;
	}
	if (count == 0) {
		smart_str_appends(b, "[]");
		return;
	}

	char hdr[32];
	int n = snprintf(hdr, sizeof hdr, "array:%u", count);

	if (mode == DD_MODE_HTML) {
		html_open_compound(b, depth, S_NOTE, hdr, n, '[', NULL, 0);
	} else {
		s_text(b, mode, S_NOTE, hdr, n);
		smart_str_appends(b, " [\n");
	}

	GC_TRY_PROTECT_RECURSION(ht);

	zend_string *key;
	zend_ulong idx;
	zval *val;
	ZEND_HASH_FOREACH_KEY_VAL(ht, idx, key, val) {
		indent(b, depth + 1);
		if (key) {
			s_open(b, mode, S_KEY);
			smart_str_appendc(b, '"');
			app_text(b, mode, ZSTR_VAL(key), ZSTR_LEN(key));
			smart_str_appendc(b, '"');
			s_close(b, mode, S_KEY);
		} else {
			char t[32];
			int m = snprintf(t, sizeof t, ZEND_LONG_FMT, (zend_long) idx);
			s_text(b, mode, S_INDEX, t, m);
		}
		smart_str_appends(b, " => ");
		dd_dump(b, val, depth + 1, mode);
		smart_str_appendc(b, '\n');
	} ZEND_HASH_FOREACH_END();

	GC_TRY_UNPROTECT_RECURSION(ht);

	indent(b, depth);
	if (mode == DD_MODE_HTML) {
		html_close_compound(b, count, ']');
	} else {
		smart_str_appendc(b, ']');
	}
}

static void dump_object(smart_str *b, zval *z, int depth, int mode)
{
	zend_object *obj = Z_OBJ_P(z);
	zend_string *cn = obj->ce->name;

	if (GC_IS_RECURSIVE(obj)) {
		s_text(b, mode, S_REF, "*RECURSION*", sizeof("*RECURSION*") - 1);
		return;
	}

	zend_array *props = zend_get_properties_for(z, ZEND_PROP_PURPOSE_DEBUG);
	uint32_t count = props ? zend_hash_num_elements(props) : 0;

	char idb[24];
	int idn = snprintf(idb, sizeof idb, "#%u", obj->handle);

	if (mode == DD_MODE_HTML && count > 0) {
		/* The object id is rendered as styled "extra" right after '{'. */
		smart_str ref = {0};
		s_text(&ref, mode, S_REF, idb, idn);
		smart_str_0(&ref);
		html_open_compound(b, depth, S_NOTE, ZSTR_VAL(cn), (int) ZSTR_LEN(cn),
			'{', ZSTR_VAL(ref.s), (int) ZSTR_LEN(ref.s));
		smart_str_free(&ref);
	} else {
		s_text(b, mode, S_NOTE, ZSTR_VAL(cn), ZSTR_LEN(cn));
		smart_str_appends(b, " {");
		s_text(b, mode, S_REF, idb, idn);
		if (count == 0) {
			smart_str_appendc(b, '}');
			if (props) {
				zend_release_properties(props);
			}
			return;
		}
		smart_str_appendc(b, '\n');
	}

	GC_TRY_PROTECT_RECURSION(obj);

	zend_string *key;
	zval *val;
	ZEND_HASH_FOREACH_STR_KEY_VAL(props, key, val) {
		indent(b, depth + 1);

		const char *un_class = NULL, *un_prop = NULL;
		size_t un_len = 0;
		char prefix = '+';
		if (key) {
			zend_unmangle_property_name_ex(key, &un_class, &un_prop, &un_len);
			if (!un_class) {
				prefix = '+'; /* public */
			} else if (un_class[0] == '*' && un_class[1] == '\0') {
				prefix = '#'; /* protected */
			} else {
				prefix = '-'; /* private */
			}
		} else {
			un_prop = "";
			un_len = 0;
		}

		smart_str_appendc(b, prefix);
		app_text(b, mode, un_prop, un_len);
		smart_str_appends(b, ": ");
		dd_dump(b, val, depth + 1, mode);
		smart_str_appendc(b, '\n');
	} ZEND_HASH_FOREACH_END();

	GC_TRY_UNPROTECT_RECURSION(obj);
	zend_release_properties(props);

	indent(b, depth);
	if (mode == DD_MODE_HTML) {
		html_close_compound(b, count, '}');
	} else {
		smart_str_appendc(b, '}');
	}
}

static void dd_dump(smart_str *b, zval *z, int depth, int mode)
{
	if (Z_TYPE_P(z) == IS_INDIRECT) {
		z = Z_INDIRECT_P(z);
	}
	ZVAL_DEREF(z);

	if (Z_TYPE_P(z) == IS_UNDEF) {
		s_text(b, mode, S_META, "uninitialized", sizeof("uninitialized") - 1);
		return;
	}

	switch (Z_TYPE_P(z)) {
		case IS_NULL:
			s_text(b, mode, S_CONST, "null", 4);
			break;
		case IS_TRUE:
			s_text(b, mode, S_CONST, "true", 4);
			break;
		case IS_FALSE:
			s_text(b, mode, S_CONST, "false", 5);
			break;
		case IS_LONG: {
			char tmp[32];
			int n = snprintf(tmp, sizeof tmp, ZEND_LONG_FMT, Z_LVAL_P(z));
			s_text(b, mode, S_NUM, tmp, n);
			break;
		}
		case IS_DOUBLE: {
			char tmp[64];
			format_double(tmp, sizeof tmp, Z_DVAL_P(z));
			s_text(b, mode, S_NUM, tmp, strlen(tmp));
			break;
		}
		case IS_STRING:
			dump_string(b, mode, Z_STR_P(z));
			break;
		case IS_ARRAY:
			dump_array(b, z, depth, mode);
			break;
		case IS_OBJECT:
			dump_object(b, z, depth, mode);
			break;
		case IS_RESOURCE: {
			char tmp[80];
			const char *tn = zend_rsrc_list_get_rsrc_type(Z_RES_P(z));
			int n = snprintf(tmp, sizeof tmp, ":%s resource @%d",
				tn ? tn : "Unknown", Z_RES_HANDLE_P(z));
			s_text(b, mode, S_NOTE, tmp, n);
			break;
		}
		default:
			s_text(b, mode, S_NOTE, "unknown", 7);
	}
}

/* ------------------------------------------------------------------ *
 * Rendering / SAPI detection
 * ------------------------------------------------------------------ */

static int dd_resolve_mode(void)
{
	const char *name = sapi_module.name;
	int is_cli = name && (!strcmp(name, "cli") ||
	                      !strcmp(name, "phpdbg") ||
	                      !strcmp(name, "embed"));
	if (!is_cli) {
		return DD_MODE_HTML;
	}
	if (getenv("NO_COLOR") == NULL && isatty(STDOUT_FILENO)) {
		return DD_MODE_ANSI;
	}
	return DD_MODE_PLAIN;
}

static void emit_html_assets(void)
{
	static const char assets[] =
		"<style>"
		"pre.sf-dump{display:block;white-space:pre;padding:5px;margin:6px 0;"
		"background:#18171B;color:#FF8400;line-height:1.3em;"
		"font:12px Menlo,Monaco,Consolas,monospace;word-break:normal;"
		"border-radius:5px;overflow:auto;}"
		"pre.sf-dump span,pre.sf-dump samp{display:inline;}"
		".sf-dump-toggle{cursor:pointer;text-decoration:none;color:inherit;}"
		".sf-dump-arrow{display:inline-block;width:1em;color:#A0A0A0;font-size:10px;"
		"vertical-align:middle;}"
		"samp.sf-dump-collapsed{display:none;}"
		".sf-dump-ellip{display:none;color:#A0A0A0;}"
		"samp.sf-dump-collapsed + .sf-dump-ellip{display:inline;}"
		".sf-dump-num{font-weight:bold;color:#1299DA}"
		".sf-dump-const{font-weight:bold}"
		".sf-dump-str{font-weight:bold;color:#56DB3A}"
		".sf-dump-note{color:#1299DA}"
		".sf-dump-ref{color:#A0A0A0}"
		".sf-dump-key{color:#56DB3A}"
		".sf-dump-index{color:#1299DA}"
		".sf-dump-meta{color:#B729D9}"
		".sf-dump-default{color:#FF8400}"
		"</style>"
		"<script>"
		"(function(){"
		"if(window.__ddDumpInit)return;window.__ddDumpInit=1;"
		"document.addEventListener('click',function(e){"
		"var t=e.target.closest?e.target.closest('.sf-dump-toggle'):null;"
		"if(!t)return;e.preventDefault();"
		"var n=t.nextElementSibling;"
		"while(n&&n.tagName!=='SAMP')n=n.nextElementSibling;"
		"if(!n)return;"
		"var a=t.querySelector('.sf-dump-arrow');"
		"if(n.classList.contains('sf-dump-collapsed')){"
		"n.classList.remove('sf-dump-collapsed');if(a)a.textContent='\\u25BC';"
		"}else{"
		"n.classList.add('sf-dump-collapsed');if(a)a.textContent='\\u25B6';"
		"}});"
		"})();"
		"</script>";
	zend_write(assets, sizeof(assets) - 1);
}

static void dd_render(zval *args, int argc)
{
	int mode = dd_resolve_mode();

	if (mode == DD_MODE_HTML && !DD_G(html_assets_dumped)) {
		emit_html_assets();
		DD_G(html_assets_dumped) = 1;
	}

	for (int i = 0; i < argc; i++) {
		smart_str buf = {0};
		if (mode == DD_MODE_HTML) {
			smart_str_appends(&buf, "<pre class=sf-dump>");
		}
		dd_dump(&buf, &args[i], 0, mode);
		if (mode == DD_MODE_HTML) {
			smart_str_appends(&buf, "</pre>\n");
		} else {
			smart_str_appendc(&buf, '\n');
		}
		smart_str_0(&buf);
		if (buf.s) {
			zend_write(ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
		}
		smart_str_free(&buf);
	}
}

/* ------------------------------------------------------------------ *
 * Userland functions
 * ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(arginfo_dd, 0, 0, 0)
	ZEND_ARG_VARIADIC_INFO(0, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_dump, 0, 0, IS_MIXED, 0)
	ZEND_ARG_VARIADIC_INFO(0, vars)
ZEND_END_ARG_INFO()

PHP_FUNCTION(dd)
{
	zval *args = NULL;
	int argc = 0;

	ZEND_PARSE_PARAMETERS_START(0, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	dd_render(args, argc);

	/* Behave like exit(1): flush happens during the bailout-driven shutdown. */
	EG(exit_status) = 1;
	zend_bailout();
}

PHP_FUNCTION(dump)
{
	zval *args = NULL;
	int argc = 0;

	ZEND_PARSE_PARAMETERS_START(0, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	dd_render(args, argc);

	if (argc == 0) {
		RETURN_NULL();
	}
	if (argc == 1) {
		RETURN_COPY(&args[0]);
	}

	array_init_size(return_value, argc);
	for (int i = 0; i < argc; i++) {
		Z_TRY_ADDREF(args[i]);
		add_next_index_zval(return_value, &args[i]);
	}
}

static const zend_function_entry dd_functions[] = {
	PHP_FE(dd, arginfo_dd)
	PHP_FE(dump, arginfo_dump)
	PHP_FE_END
};

/* ------------------------------------------------------------------ *
 * Module lifecycle
 * ------------------------------------------------------------------ */

static PHP_GINIT_FUNCTION(dd)
{
#if defined(COMPILE_DL_DD) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	dd_globals->html_assets_dumped = 0;
}

PHP_RINIT_FUNCTION(dd)
{
	DD_G(html_assets_dumped) = 0;
	return SUCCESS;
}

PHP_MINFO_FUNCTION(dd)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "dd support", "enabled");
	php_info_print_table_row(2, "Version", PHP_DD_VERSION);
	php_info_print_table_row(2, "Functions", "dd(), dump()");
	php_info_print_table_end();
}

zend_module_entry dd_module_entry = {
	STANDARD_MODULE_HEADER,
	"dd",
	dd_functions,
	NULL,                 /* MINIT     */
	NULL,                 /* MSHUTDOWN */
	PHP_RINIT(dd),        /* RINIT     */
	NULL,                 /* RSHUTDOWN */
	PHP_MINFO(dd),        /* MINFO     */
	PHP_DD_VERSION,
	PHP_MODULE_GLOBALS(dd),
	PHP_GINIT(dd),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_DD
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(dd)
#endif
