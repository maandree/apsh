/* See LICENSE file for copyright and license details. */
#include "common.h"


void
push_mode(struct parser_context *ctx, enum tokeniser_mode mode)
{
	struct mode_stack *new = emalloc(sizeof(*new));
	new->mode = mode;
	new->she_is_comment = 1;
	new->previous = ctx->mode_stack;
	ctx->mode_stack = new;
}


void
pop_mode(struct parser_context *ctx)
{
	struct mode_stack *old = ctx->mode_stack;
	ctx->mode_stack = ctx->mode_stack->previous;
	free(old);
}


size_t
parse_preparsed(struct parser_context *ctx, char *code, size_t code_len)
{
#define IS_SYMBOL(C) ((C) == '<' || (C) == '>' || (C) == '&' || (C) == '|' ||\
                      (C) == '(' || (C) == ')' || (C) == ';' || (C) == '-')

	size_t bytes_read = 0;
	size_t token_len;

	for (; bytes_read < code_len; bytes_read += token_len, code = &code[token_len]) {
		switch (ctx->mode_stack->mode) {
		case NORMAL_MODE:
			if (*code == '#' && ctx->mode_stack->she_is_comment) {
				token_len = 1;
				push_mode(ctx, COMMENT_MODE);

			} else if (*code == '\n') {
				token_len = 1;
				ctx->mode_stack->she_is_comment = 1;
				push_whitespace(ctx, 0);
				push_semicolon(ctx, 1);
				ctx->tokeniser_line_number += 1;
				if (ctx->here_documents_first)
					push_mode(ctx, HERE_DOCUMENT_MODE);

			} else if (isspace(*code)) {
				ctx->mode_stack->she_is_comment = 1;
				push_whitespace(ctx, 0);
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1)
					if (!isspace(code[token_len]) || code[token_len] == '\n')
						break;

			} else if (*code == ')' && ctx->mode_stack->previous) {
				token_len = 1;
				ctx->mode_stack->she_is_comment = 1;
				pop_mode(ctx);
				push_leave(ctx);

			} else if (IS_SYMBOL(*code)) {
				ctx->mode_stack->she_is_comment = 1;
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1)
					if (!IS_SYMBOL(code[token_len]))
						goto symbol_end;
				if (!ctx->end_of_file_reached)
					goto need_more;
			symbol_end:
				token_len = push_symbol(ctx, code, token_len);

			} else if (*code == '\\') {
				ctx->mode_stack->she_is_comment = 0;
			backslash_mode:
				if (code_len - bytes_read < 2)
					goto need_more;
				token_len = 2;
				push_quoted(ctx, &code[1], 1);

			} else if (*code == '\'') {
				ctx->mode_stack->she_is_comment = 0;
			sqoute_mode:
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1)
					if (code[token_len] == '\'')
						goto squote_end;
				goto need_more;
			squote_end:
				token_len += 1;
				push_quoted(ctx, &code[1], token_len - 2);

			} else if (*code == '"') {
				ctx->mode_stack->she_is_comment = 0;
			dquote_mode:
				token_len = 1;
				push_mode(ctx, DQ_QUOTE_MODE);
				push_enter(ctx, QUOTE_EXPRESSION);

			} else if (*code == '`') {
				ctx->mode_stack->she_is_comment = 0;
			bquote_mode:
				token_len = 1;
				push_mode(ctx, BQ_QUOTE_MODE);
				push_enter(ctx, BACKQUOTE_EXPRESSION);

			} else if (*code == '$') {
				ctx->mode_stack->she_is_comment = 0;
			dollar_mode:
				if (code_len - bytes_read < 2) {
					if (ctx->end_of_file_reached) {
						token_len = 1;
						push_unquoted(ctx, code, 1);
					} else {
						goto need_more;
					}

				} else if (code[1] == '(') {
					if (code_len - bytes_read < 3) {
						goto need_more;

					} else if (code[2] == '(') {
						token_len = 3;
						push_mode(ctx, RRB_QUOTE_MODE);
						push_enter(ctx, ARITHMETIC_EXPRESSION);

					} else {
						token_len = 2;
						push_mode(ctx, NORMAL_MODE);
						push_enter(ctx, SUBSHELL_SUBSTITUTION);
					}

				} else if (code[1] == '[') {
					token_len = 2;
					push_mode(ctx, SB_QUOTE_MODE);
					push_enter(ctx, ARITHMETIC_EXPRESSION);

				} else if (code[1] == '{') {
					token_len = 2;
					push_mode(ctx, CB_QUOTE_MODE);
					push_enter(ctx, VARIABLE_SUBSTITUTION);

				} else if (code[1] == '\'') {
					for (token_len = 2; token_len < code_len - bytes_read; token_len += 1) {
						if (code[token_len] == '\\') {
							if (token_len + 1 == code_len - bytes_read) {
								token_len += 1;
							} else {
								goto need_more;
							}
						} else if (code[token_len] == '\'') {
							goto dollar_squote_end;
						}
					}
				dollar_squote_end:
					token_len += 1;
					push_escaped(ctx, &code[2], token_len - 3);

				} else {
					token_len = 1;
					push_unquoted(ctx, code, 1);
				}

			} else {
				ctx->mode_stack->she_is_comment = 0;
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1) {
					if (isspace(code[token_len]) || IS_SYMBOL(code[token_len]) ||
					    code[token_len] == '\''  || code[token_len] == '"'     ||
					    code[token_len] == '\\'  || code[token_len] == '$'     ||
					    code[token_len] == '`')
						break;
				}
				push_unquoted(ctx, code, token_len);
			}
			break;


		case COMMENT_MODE:
			if (*code == '\n') {
				token_len = 0; /* do not consume */
				pop_mode(ctx);
			} else {
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1)
					if (code[token_len] == '\n')
						break;
			}
			break;


		case HERE_DOCUMENT_MODE:
			/* TODO read until terminator, remove all <tab> (including on the
			 *      line of the terminator) if <<- and then if terminator was
			 *      unquoted, parse in " "-mode but accept " */
			break;


		case BQ_QUOTE_MODE:
			if (*code == '\\') {
				if (code_len - bytes_read < 2) {
					goto need_more;
				} else {
					token_len = 2;
					push_unquoted(ctx, code, 2);
				}

			} else if (*code == '`') {
				token_len = 1;
				pop_mode(ctx);
				push_leave(ctx);

			} else if (*code == '\n') {
				token_len = 1;
				ctx->tokeniser_line_number += 1;
				push_unquoted(ctx, code, 1);

			} else {
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1)
					if (code[token_len] == '\n' || code[token_len] == '\\' || code[token_len] == '`')
						break;
				push_unquoted(ctx, code, token_len);
			}
			break;


		case DQ_QUOTE_MODE:
			if (*code == '"') {
				token_len = 1;
				pop_mode(ctx);
				push_leave(ctx);
			} else {
				goto common_quote_mode;
			}
			break;

		case RRB_QUOTE_MODE:
			if (*code == ')') {
				if (code_len - bytes_read < 2) {
					goto need_more;
				} else if (code[1] == ')') {
					token_len = 2;
					pop_mode(ctx);
					push_leave(ctx);
				} else {
					goto common_quote_mode;
				}
			} else {
				goto common_quote_mode;
			}
			break;

		case RB_QUOTE_MODE:
			if (*code == ')') {
				token_len = 1;
				pop_mode(ctx);
				push_leave(ctx);
			} else {
				goto common_quote_mode;
			}
			break;

		case SB_QUOTE_MODE:
			if (*code == ']') {
				token_len = 1;
				pop_mode(ctx);
				push_leave(ctx);
			} else {
				goto common_quote_mode;
			}
			break;

		common_quote_mode:
			if (*code == '(' && ctx->mode_stack->mode != DQ_QUOTE_MODE) {
				if (code_len - bytes_read < 2) {
					goto need_more;

				} else if (code[1] == '(') {
					token_len = 2;
					push_mode(ctx, RRB_QUOTE_MODE);
					push_enter(ctx, ARITHMETIC_EXPRESSION);

				} else {
					token_len = 1;
					push_mode(ctx, RB_QUOTE_MODE);
					push_enter(ctx, ARITHMETIC_EXPRESSION);
				}

			} else if (*code == '$') {
				if (code_len - bytes_read < 2) {
					if (ctx->end_of_file_reached) {
						token_len = 1;
						push_unquoted(ctx, code, 1);
					} else {
						goto need_more;
					}

				} else if (code[1] == '(') {
					if (code_len - bytes_read < 3) {
						goto need_more;

					} else if (code[2] == '(') {
						token_len = 3;
						push_mode(ctx, RRB_QUOTE_MODE);
						push_enter(ctx, ARITHMETIC_EXPRESSION);

					} else {
						token_len = 2;
						push_mode(ctx, NORMAL_MODE);
						push_enter(ctx, SUBSHELL_SUBSTITUTION);
					}

				} else if (code[1] == '[') {
					token_len = 2;
					push_mode(ctx, SB_QUOTE_MODE);
					push_enter(ctx, ARITHMETIC_EXPRESSION);

				} else if (code[1] == '{') {
					token_len = 2;
					push_mode(ctx, CB_QUOTE_MODE);
					push_enter(ctx, VARIABLE_SUBSTITUTION);

				} else {
					token_len = 1;
					push_unquoted(ctx, code, 1);
				}

			} else if (*code == '\\') {
				if (code_len - bytes_read < 2) {
					if (ctx->end_of_file_reached) {
						token_len = 1;
						push_unquoted(ctx, code, 1);
					} else {
						goto need_more;
					}

				} else if (code[1] == '$' || code[1] == '`' || code[1] == '"' || code[1] == '\\') {
					token_len = 1;
					push_quoted(ctx, &code[1], 1);

				} else {
					token_len = 1;
					push_unquoted(ctx, code, 1);
				}

			} else if (*code == '`') {
				goto bquote_mode;

			} else if (*code == '\n') {
				token_len = 1;
				ctx->tokeniser_line_number += 1;
				push_unquoted(ctx, code, 1);

			} else {
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1) {
					if (code[token_len] == '"' || code[token_len] == ')'  ||
					    code[token_len] == ']' || code[token_len] == '('  ||
					    code[token_len] == '$' || code[token_len] == '\\' ||
					    code[token_len] == '`' || code[token_len] == '\n')
						break;
				}
				push_unquoted(ctx, code, token_len);
			}
			break;


		case CB_QUOTE_MODE:
			if (*code == '}') {
				token_len = 1;
				pop_mode(ctx);
				push_leave(ctx);

			} else if (*code == '\\') {
				goto backslash_mode;

			} else if (*code == '\'') {
				goto sqoute_mode;

			} else if (*code == '"') {
				goto dquote_mode;

			} else if (*code == '`') {
				goto bquote_mode;

			} else if (*code == '$') {
				goto dollar_mode;

			} else if (*code == '\n') {
				token_len = 1;
				ctx->tokeniser_line_number += 1;
				push_unquoted(ctx, code, 1);

			} else {
				for (token_len = 1; token_len < code_len - bytes_read; token_len += 1) {
					if (code[token_len] == '}'  || code[token_len] == '\\' ||
					    code[token_len] == '\'' || code[token_len] == '"'  ||
					    code[token_len] == '`'  || code[token_len] == '$'  ||
					    code[token_len] == '\n')
						break;
				}
				push_unquoted(ctx, code, token_len);
			}
			break;

		default:
			abort();
		}

		if (ctx->line_continuations) {
			ctx->tokeniser_line_number += ctx->line_continuations;
			ctx->line_continuations = 0;
		}
	}

	if (bytes_read == code_len && ctx->end_of_file_reached)
		push_end_of_file(ctx);

need_more:
	return bytes_read;

#undef IS_SYMBOL
}
