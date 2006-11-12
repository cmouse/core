/* Copyright (C) 2003-2004 Timo Sirainen */

#include "lib.h"
#include "md5.h"
#include "hash.h"
#include "hex-binary.h"
#include "str.h"
#include "strescape.h"
#include "var-expand.h"

#include <stdlib.h>

struct var_expand_context {
	int offset;
	unsigned int width;
	bool zero_padding;
};

struct var_expand_modifier {
	char key;
	const char *(*func)(const char *, struct var_expand_context *);
};

static const char *
m_str_lcase(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	return t_str_lcase(str);
}

static const char *
m_str_ucase(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	return t_str_ucase(str);
}

static const char *
m_str_escape(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	return str_escape(str);
}

static const char *
m_str_hex(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	unsigned long long l;

	l = strtoull(str, NULL, 10);
	return t_strdup_printf("%llx", l);
}

static const char *
m_str_reverse(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	size_t len = strlen(str);
	char *p, *rev;

	rev = t_malloc(len + 1);
	rev[len] = '\0';

	for (p = rev + len - 1; *str != '\0'; str++)
		*p-- = *str;
	return rev;
}

static const char *m_str_hash(const char *str, struct var_expand_context *ctx)
{
	unsigned int value = str_hash(str);
	string_t *hash = t_str_new(20);

	if (ctx->width != 0) {
		value %= ctx->width;
		ctx->width = 0;
	}

	str_printfa(hash, "%x", value);
	while ((int)str_len(hash) < ctx->offset)
		str_insert(hash, 0, "0");
        ctx->offset = 0;

	return str_c(hash);
}

static const char *m_str_md5(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	unsigned char digest[16];

	md5_get_digest(str, strlen(str), digest);

	return binary_to_hex(digest, sizeof(digest));
}

static const char *m_str_ldap_dn(const char *str, struct var_expand_context *ctx __attr_unused__)
{
	string_t *ret = t_str_new(256);

	while (*str) {
		if (*str == '.')
			str_append(ret, ",dc=");
		else
			str_append_c(ret, *str);
		str++;
	}

	return str_free_without_data(&ret);
}

#define MAX_MODIFIER_COUNT 10
static const struct var_expand_modifier modifiers[] = {
	{ 'L', m_str_lcase },
	{ 'U', m_str_ucase },
	{ 'E', m_str_escape },
	{ 'X', m_str_hex },
	{ 'R', m_str_reverse },
	{ 'H', m_str_hash },
	{ 'M', m_str_md5 },
	{ 'D', m_str_ldap_dn },
	{ '\0', NULL }
};

void var_expand(string_t *dest, const char *str,
		const struct var_expand_table *table)
{
        const struct var_expand_modifier *m;
        const struct var_expand_table *t;
	const char *var;
        struct var_expand_context ctx;
	const char *(*modifier[MAX_MODIFIER_COUNT])
		(const char *, struct var_expand_context *);
	unsigned int i, modifier_count;

	memset(&ctx, 0, sizeof(ctx));
	for (; *str != '\0'; str++) {
		if (*str != '%')
			str_append_c(dest, *str);
		else {
			int sign = 1;

			str++;
			memset(&ctx, 0, sizeof(ctx));

			/* [<offset>.]<width>[<modifiers>]<variable> */
			if (*str == '-') {
				sign = -1;
				str++;
			}
			if (*str == '0') {
				ctx.zero_padding = TRUE;
				str++;
			}
			while (*str >= '0' && *str <= '9') {
				ctx.width = ctx.width*10 + (*str - '0');
				str++;
			}

			if (*str == '.') {
				ctx.offset = sign * (int)ctx.width;
				ctx.width = 0;
				str++;

				/* if offset was prefixed with zero (or it was
				   plain zero), just ignore that. zero padding
				   is done with the width. */
				ctx.zero_padding = FALSE;
				if (*str == '0') {
					ctx.zero_padding = TRUE;
					str++;
				}

				while (*str >= '0' && *str <= '9') {
					ctx.width = ctx.width*10 + (*str - '0');
					str++;
				}
			}

                        modifier_count = 0;
			while (modifier_count < MAX_MODIFIER_COUNT) {
				modifier[modifier_count] = NULL;
				for (m = modifiers; m->key != '\0'; m++) {
					if (m->key == *str) {
						/* @UNSAFE */
						modifier[modifier_count] =
							m->func;
						str++;
						break;
					}
				}
				if (modifier[modifier_count] == NULL)
					break;
				modifier_count++;
			}

			if (*str == '\0')
				break;

			var = NULL;
			for (t = table; t->key != '\0'; t++) {
				if (t->key == *str) {
					var = t->value != NULL ? t->value : "";
					break;
				}
			}

			if (var == NULL) {
				/* not found */
				if (*str == '%')
					var = "%";
			}

			if (var != NULL) {
				for (i = 0; i < modifier_count; i++)
					var = modifier[i](var, &ctx);

				if (ctx.offset < 0) {
					/* if offset is < 0 then we want to
					   start at the end */
					size_t len = strlen(var);

					if (len > (size_t)-ctx.offset)
						var += len + ctx.offset;
				} else {
					while (*var != '\0' && ctx.offset > 0) {
						ctx.offset--;
						var++;
					}
				}
				if (ctx.width == 0)
					str_append(dest, var);
				else if (!ctx.zero_padding)
					str_append_n(dest, var, ctx.width);
				else {
					/* %05d -like padding. no truncation. */
					size_t len = strlen(var);
					while (len < ctx.width) {
						str_append_c(dest, '0');
						ctx.width--;
					}
					str_append(dest, var);
				}
			}
		}
	}
}

char var_get_key(const char *str)
{
	const struct var_expand_modifier *m;

	/* [<offset>.]<width>[<modifiers>]<variable> */
	while ((*str >= '0' && *str <= '9') || *str == '-')
		str++;

	if (*str == '.') {
		str++;
		while (*str >= '0' && *str <= '9')
			str++;
	}

	do {
		for (m = modifiers; m->key != '\0'; m++) {
			if (m->key == *str) {
				str++;
				break;
			}
		}
	} while (m->key != '\0');

	return *str;
}
