/* See LICENSE file for copyright and license details. */
#include "common.h"


void
push_mode(struct parser_context *ctx, enum tokeniser_mode mode)
{
	struct mode_stack *new_mode_stack;
	struct here_document_stack *new_here_document_stack;

	if (mode == BQ_QUOTE_MODE)
		weprintf("backquote expression found at line %zu, stop it!\n", ctx->tokeniser_line_number);

	if (ctx->mode_stack->mode == HERE_DOCUMENT_MODE) {
		new_here_document_stack = ecalloc(1, sizeof(*new_here_document_stack));
		new_here_document_stack->next = &new_here_document_stack->first;
		new_here_document_stack->previous = ctx->here_document_stack;
		ctx->here_document_stack = new_here_document_stack;
	}

	new_mode_stack = emalloc(sizeof(*new_mode_stack));
	new_mode_stack->mode = mode;
	new_mode_stack->she_is_comment = 1;
	new_mode_stack->previous = ctx->mode_stack;
	ctx->mode_stack = new_mode_stack;
}


void
pop_mode(struct parser_context *ctx)
{
	struct mode_stack *old_mode_stack;
	struct here_document_stack *old_here_document_stack;
	struct here_document_stack *prev_here_document_stack;

	old_mode_stack = ctx->mode_stack;
	ctx->mode_stack = ctx->mode_stack->previous;
	free(old_mode_stack);

	if (ctx->mode_stack->mode == HERE_DOCUMENT_MODE) {
		if (ctx->here_document_stack->first) {
			if (posix_mode) {
				eprintf("subshell expression closed at line %zu before here-documents, "
				        "this is non-portable\n", ctx->tokeniser_line_number);
			}
			prev_here_document_stack = ctx->here_document_stack->previous;
			*ctx->here_document_stack->next = prev_here_document_stack->first;
			ctx->here_document_stack->next = prev_here_document_stack->next;
			ctx->here_document_stack->previous = prev_here_document_stack->previous;
			ctx->here_document_stack->interpret_when_empty = prev_here_document_stack->interpret_when_empty;
			free(prev_here_document_stack);
		} else {
			old_here_document_stack = ctx->here_document_stack;
			ctx->here_document_stack = old_here_document_stack->previous;
			free(old_here_document_stack);
		}
	}
}


static void
append_and_destroy_quote_to_here_document_terminator(struct here_document *here_document, struct parser_state *quote)
{
	struct argument *terminator, *part, *next_part;
	size_t i;

	terminator = here_document->argument->next_part;

	for (i = 0; i < quote->narguments; i++) {
		for (part = quote->arguments[i]; part; part = next_part) {
			next_part = part->next_part;
			if (part->type != QUOTED && part->type != UNQUOTED) {
				eprintf("use of run-time evaluated expression as right-hand side "
				        "of %s operator (at line %zu) is illegal\n",
				        here_document->redirection->type == HERE_DOCUMENT_INDENTED ? "<<-" : "<<",
				        here_document->argument->line_number);
			}
			terminator->text = erealloc(terminator->text, terminator->length + part->length + 1);
			memcpy(&terminator->text[terminator->length], part->text, part->length);
			terminator->length += part->length;
			terminator->text[terminator->length] = '\0';
			free(part->text);
			free(part);
		}
	}

	free(quote->arguments);
}

static void
get_here_document_terminator(struct parser_context *ctx)
{
	struct argument *terminator, *next_part;
	struct parser_state *child;

	terminator = ctx->here_document_stack->first->argument->next_part;
	if (!terminator || (terminator->type != QUOTED && terminator->type != UNQUOTED && terminator->type != QUOTE_EXPRESSION)) {
		eprintf("missing right-hand side of %s operator at line %zu\n",
		        ctx->here_document_stack->first->redirection->type == HERE_DOCUMENT_INDENTED ? "<<-" : "<<",
		        ctx->here_document_stack->first->argument->line_number);
	} else if (terminator->type == QUOTE_EXPRESSION) {
		child = terminator->child;
		terminator->type = QUOTED;
		terminator->text = ecalloc(1, 1);
		terminator->length = 0;
		append_and_destroy_quote_to_here_document_terminator(ctx->here_document_stack->first, child);
		free(child);
	}

	while ((next_part = terminator->next_part)) {
		switch (next_part->type) {
		case QUOTED:
			terminator->type = QUOTED;
			/* fall through */
		case UNQUOTED:
			terminator->text = erealloc(terminator->text, terminator->length + next_part->length + 1);
			memcpy(&terminator->text[terminator->length], next_part->text, next_part->length);
			terminator->length += next_part->length;
			terminator->text[terminator->length] = '\0';
			free(next_part->text);
			break;

		case QUOTE_EXPRESSION:
			terminator->type = QUOTED;
			append_and_destroy_quote_to_here_document_terminator(ctx->here_document_stack->first, next_part->child);
			free(next_part->child);
			break;

		case BACKQUOTE_EXPRESSION:
		case ARITHMETIC_EXPRESSION:
		case VARIABLE_SUBSTITUTION:
		case SUBSHELL_SUBSTITUTION:
		case PROCESS_SUBSTITUTION_INPUT:
		case PROCESS_SUBSTITUTION_OUTPUT:
		case PROCESS_SUBSTITUTION_INPUT_OUTPUT:
			eprintf("use of run-time evaluated expression as right-hand side of %s operator (at line %zu) is illegal\n",
			        ctx->here_document_stack->first->redirection->type == HERE_DOCUMENT_INDENTED ? "<<-" : "<<",
			        ctx->here_document_stack->first->argument->line_number);
			return;

		case REDIRECTION:
		case FUNCTION_MARK:
		case SUBSHELL:
		case ARITHMETIC_SUBSHELL:
			/* interpreter shall recognise these as new "arguments" */
			return;

		default:
		case COMMAND: /* used by interpreter */
		case VARIABLE: /* ditto */
			abort();
		}

		if (ctx->parser_state->current_argument_end == next_part)
			ctx->parser_state->current_argument_end = terminator;
		terminator->next_part = next_part->next_part;
		free(next_part);
	}
}


int
check_extension(const char *token, size_t line_number)
{
	if (!posix_mode) {
		return 1;
	} else {
		weprintf("the '%s' token (at line %zu) is not portable, not parsing as it\n", token, line_number);
		return 0;
	}
}


size_t
parse_preparsed(struct parser_context *ctx, char *code, size_t code_len)
{
#define IS_SYMBOL(C) ((C) == '<' || (C) == '>' || (C) == '&' || (C) == '|' ||\
                      (C) == '(' || (C) == ')' || (C) == ';' || (C) == '-')

	size_t bytes_read = 0;
	size_t token_len;
	struct here_document *here_document;
	struct here_document_stack *here_doc_stack;

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
				if (ctx->here_document_stack->first)
					push_mode(ctx, HERE_DOCUMENT_MODE_INITIALISATION);

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

				} else if (code[1] == '[' && check_extension("$[", ctx->tokeniser_line_number)) {
					token_len = 2;
					push_mode(ctx, SB_QUOTE_MODE);
					push_enter(ctx, ARITHMETIC_EXPRESSION);

				} else if (code[1] == '{') {
					token_len = 2;
					push_mode(ctx, CB_QUOTE_MODE);
					push_enter(ctx, VARIABLE_SUBSTITUTION);

				} else if (code[1] == '\'' && check_extension("$'", ctx->tokeniser_line_number)) {
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


		case HERE_DOCUMENT_MODE_INITIALISATION:
			here_doc_stack = ctx->here_document_stack;
			here_doc_stack->indented = 0;
			if (here_doc_stack->first->redirection->type == HERE_DOCUMENT_INDENTED)
				here_doc_stack->indented = 1;
			get_here_document_terminator(ctx);
			here_doc_stack->verbatim = 0;
			if (here_doc_stack->first->argument->next_part->type == QUOTED)
				here_doc_stack->verbatim = 1;
			here_doc_stack->first->terminator = here_doc_stack->first->argument->next_part->text;
			here_doc_stack->first->terminator_length = here_doc_stack->first->argument->next_part->length;
			here_doc_stack->first->argument->next_part->text = ecalloc(1, 1);
			here_doc_stack->first->argument->next_part->length = 0;
			here_doc_stack->first->argument->next_part->type = QUOTED;
			here_doc_stack->first->argument_end = here_doc_stack->first->argument->next_part;
			ctx->mode_stack->mode = HERE_DOCUMENT_MODE;
			/* fall through */

		case HERE_DOCUMENT_MODE:
			here_doc_stack = ctx->here_document_stack;
			if (*code == '\t' && here_doc_stack->indented) {
				token_len = 1;
			} else {
				token_len = here_doc_stack->line_offset;
				for (; token_len < code_len - bytes_read; token_len += 1) {
					if (code[token_len] == '\n') {
						goto here_document_line_end;
					} else if (!here_doc_stack->verbatim) {
						if (code[token_len] == '\\') {
							if (token_len + 1 == code_len - bytes_read) {
								goto need_more;
							} else if (code[token_len + 1] == '$' || code[token_len + 1] == '`') {
								here_doc_stack->line_offset = 0;
								push_quoted(ctx, code, token_len);
								push_quoted(ctx, &code[token_len + 1], 1);
								goto next;
							}
							token_len += 1;
						} else if (code[token_len] == '$') {
							here_doc_stack->line_offset = 0;
							push_quoted(ctx, code, token_len);
							bytes_read += token_len;
							code = &code[token_len];
							goto quote_mode_dollar_mode;
						} else if (code[token_len] == '`') {
							here_doc_stack->line_offset = 0;
							push_quoted(ctx, code, token_len);
							push_mode(ctx, BQ_QUOTE_MODE);
							push_enter(ctx, BACKQUOTE_EXPRESSION);
							goto next;
						}
					}
				}
				goto need_more;

			here_document_line_end:
				token_len += 1;
				ctx->tokeniser_line_number += 1;
				here_doc_stack->line_offset = 0;
				here_document = here_doc_stack->first;

				if (token_len - 1 == here_document->terminator_length &&
				    !strncmp(code, here_document->terminator, token_len - 1)) {
					here_document->redirection->type = HERE_STRING;
					here_doc_stack->first = here_document->next;
					free(here_document->terminator);
					free(here_document);
					if (here_doc_stack->first) {
						ctx->mode_stack->mode = HERE_DOCUMENT_MODE_INITIALISATION;
					} else {
						here_doc_stack->next = &here_doc_stack->first;
						pop_mode(ctx);
						if (here_doc_stack->interpret_when_empty) {
							here_doc_stack->interpret_when_empty = 0;
							interpret_and_eliminate(ctx);
						}
					}
				} else {
					push_quoted(ctx, code, token_len);
				}
			}
			break;


		case BQ_QUOTE_MODE:
			if (*code == '\\') {
				if (code_len - bytes_read < 2) {
					goto need_more;
				} else if (code[1] == '\\' || code[1] == '`' || code[1] == '$') {
					token_len = 2;
					push_unquoted(ctx, &code[1], 1);
					if (code[1] == '$') {
						weprintf("meaningless \\ found before $ inside backquote expression at line "
						         "%zu, perhaps you mean to use \\\\$ instead to get a literal $\n",
						         ctx->tokeniser_line_number);
					}
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
			quote_mode_dollar_mode:
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

				} else if (code[1] == '[' && check_extension("$[", ctx->tokeniser_line_number)) {
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

	next:
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
