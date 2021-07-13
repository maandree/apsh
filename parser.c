/* See LICENSE file for copyright and license details. */
#include "common.h"


const char *
get_redirection_token(enum redirection_type type)
{
	switch (type) {
	case REDIRECT_INPUT:
		return "<";
	case REDIRECT_INPUT_TO_FD:
		return "<&";
	case REDIRECT_OUTPUT:
		return ">";
	case REDIRECT_OUTPUT_APPEND:
		return ">>";
	case REDIRECT_OUTPUT_CLOBBER:
		return ">|";
	case REDIRECT_OUTPUT_TO_FD:
		return ">&";
	case REDIRECT_OUTPUT_AND_STDERR:
		return "&>";
	case REDIRECT_OUTPUT_AND_STDERR_APPEND:
		return "&>>";
	case REDIRECT_OUTPUT_AND_STDERR_CLOBBER:
		return "&>|";
	case REDIRECT_OUTPUT_AND_STDERR_TO_FD:
		return "&>&";
	case REDIRECT_INPUT_OUTPUT:
		return "<>";
	case REDIRECT_INPUT_OUTPUT_TO_FD:
		return "<>&";
	case HERE_STRING:
		return "<<<";
	case HERE_DOCUMENT:
		return "<<";
	case HERE_DOCUMENT_INDENTED:
		return "<<-";
	default:
		abort();
	}
}


void
push_end_of_file(struct parser_context *ctx)
{
	push_semicolon(ctx, 1);
	if (ctx->parser_state->parent || ctx->parser_state->ncommands)
		ctx->premature_end_of_file = 1;
}


void
push_whitespace(struct parser_context *ctx, int strict)
{
	if (ctx->parser_state->need_right_hand_side) {
		if (strict)
			eprintf("premature end of command\n");
		return;
	}

	if (ctx->parser_state->current_argument) {
		ctx->parser_state->arguments = erealloc(ctx->parser_state->arguments,
		                                        (ctx->parser_state->narguments + 1) *
		                                        sizeof(*ctx->parser_state->arguments));
		ctx->parser_state->arguments[ctx->parser_state->narguments++] = ctx->parser_state->current_argument;
		ctx->parser_state->current_argument = NULL;
		ctx->parser_state->current_argument_end = NULL;
	}
}


static void
push_command_terminal(struct parser_context *ctx, enum command_terminal terminal)
{
	struct command *new_command;

	push_whitespace(ctx, 1);

	ctx->parser_state->commands = erealloc(ctx->parser_state->commands,
	                                       (ctx->parser_state->ncommands + 1) *
	                                       sizeof(*ctx->parser_state->commands));
	new_command = ecalloc(1, sizeof(*new_command));
	ctx->parser_state->commands[ctx->parser_state->ncommands++] = new_command;
	new_command->terminal = terminal;
	new_command->terminal_line_number = ctx->tokeniser_line_number;
	new_command->arguments = ctx->parser_state->arguments;
	new_command->narguments = ctx->parser_state->narguments;
	new_command->redirections = ctx->parser_state->redirections;
	new_command->nredirections = ctx->parser_state->nredirections;
	ctx->parser_state->arguments = NULL;
	ctx->parser_state->narguments = 0;
	ctx->parser_state->redirections = NULL;
	ctx->parser_state->nredirections = 0;

	if (!ctx->parser_state->parent && !ctx->do_not_run)
		if (terminal == DOUBLE_SEMICOLON || terminal == SEMICOLON || terminal == NEWLINE || terminal == AMPERSAND)
			interpret_and_eliminate(ctx);
}


void
push_semicolon(struct parser_context *ctx, int actually_newline)
{
	if (!actually_newline || ctx->parser_state->narguments)
		push_command_terminal(ctx, actually_newline ? NEWLINE : SEMICOLON);
}


static void
push_new_argument_part(struct parser_context *ctx, enum argument_type type)
{
	struct argument *new_part;

	new_part = ecalloc(1, sizeof(*new_part));
	new_part->type = type;
	new_part->line_number = ctx->tokeniser_line_number;

	if (ctx->mode_stack->mode == HERE_DOCUMENT_MODE) {
		ctx->here_document_stack->first->argument_end->next_part = new_part;
		ctx->here_document_stack->first->argument_end = new_part;
	} else if (ctx->parser_state->current_argument_end) {
		ctx->parser_state->current_argument_end->next_part = new_part;
		ctx->parser_state->current_argument_end = new_part;
	} else {
		ctx->parser_state->current_argument = new_part;
		ctx->parser_state->current_argument_end = new_part;
	}
}


PURE_FUNC
static int
is_numeric_argument(struct argument *argument)
{
	char *p;

	do {
		if (argument->type != UNQUOTED)
			return 0;

		for (p = argument->text; *p; p++)
			if (!isdigit(*p))
				return 0;

	} while ((argument = argument->next_part));

	return 1;
}


PURE_FUNC
static int
is_variable_reference(struct argument *argument)
{
	char *p;

	if (argument->type != UNQUOTED || isdigit(argument->text[0]) || argument->text[0] == '$')
		return 0;

	do {
		if (argument->type != UNQUOTED)
			return 0;

		for (p = argument->text; *p; p++)
			if (!isalnum(*p) && *p != '_')
				return p[0] == '$' && !p[1] && !argument->next_part;

	} while ((argument = argument->next_part));

	return 0;
}


static void
push_redirection(struct parser_context *ctx, enum redirection_type type)
{
	struct redirection *new_redirection;
	struct argument *new_argument;
	struct here_document *new_here_document;

	new_redirection = ecalloc(1, sizeof(*new_redirection));
	new_redirection->type = type;

	ctx->parser_state->redirections = erealloc(ctx->parser_state->redirections,
	                                           (ctx->parser_state->nredirections + 1) *
	                                           sizeof(*ctx->parser_state->redirections));
	ctx->parser_state->redirections[ctx->parser_state->nredirections++] = new_redirection;

	if (ctx->parser_state->current_argument) {
		if (ctx->parser_state->current_argument->type == REDIRECTION ||
		    ctx->parser_state->current_argument_end->type == QUOTED ||
		    ctx->parser_state->current_argument_end->type == QUOTE_EXPRESSION ||
		    type == REDIRECT_OUTPUT_AND_STDERR ||
		    type == REDIRECT_OUTPUT_AND_STDERR_APPEND ||
		    type == REDIRECT_OUTPUT_AND_STDERR_CLOBBER ||
		    type == REDIRECT_OUTPUT_AND_STDERR_TO_FD ||
		    !is_numeric_argument(ctx->parser_state->current_argument)) {
			if (is_variable_reference(ctx->parser_state->current_argument)) {
				if (posix_mode) {
					weprintf("the '$%s' token (at line %zu) is not portable, not parsing as it\n",
					         get_redirection_token(type), ctx->tokeniser_line_number);
				} else {
					goto argument_is_left_hand_side;
				}
			}
			push_whitespace(ctx, 1);
		} else {
		argument_is_left_hand_side:
			new_redirection->left_hand_side = ctx->parser_state->current_argument;
		}
	}

	new_argument = ecalloc(1, sizeof(*new_argument));
	new_argument->type = REDIRECTION;
	new_argument->line_number = ctx->tokeniser_line_number;
	ctx->parser_state->current_argument = new_argument;

	if (type == HERE_DOCUMENT || type == HERE_DOCUMENT_INDENTED) {
		new_here_document = emalloc(sizeof(*new_here_document));
		new_here_document->redirection = new_redirection;
		new_here_document->argument = new_argument;
		new_here_document->next = NULL;
		*ctx->here_document_stack->next = new_here_document;
		ctx->here_document_stack->next = &new_here_document->next;
	}

	ctx->parser_state->need_right_hand_side = 1;
}


static void
push_shell_io(struct parser_context *ctx, enum argument_type type, enum tokeniser_mode mode)
{
	push_mode(ctx, mode);
	push_enter(ctx, type);
}


static void
push_function_mark(struct parser_context *ctx)
{
	push_whitespace(ctx, 1);
	push_new_argument_part(ctx, FUNCTION_MARK);
	push_whitespace(ctx, 1);
}


size_t
push_symbol(struct parser_context *ctx, char *token, size_t token_len)
{
#define LIST_SYMBOLS(_)\
	_(0, "<<<", push_redirection(ctx, HERE_STRING))\
	_(1, "<<-", push_redirection(ctx, HERE_DOCUMENT_INDENTED))\
	_(0, "<>(", push_shell_io(ctx, PROCESS_SUBSTITUTION_INPUT_OUTPUT, NORMAL_MODE))\
	_(0, "<>|", push_command_terminal(ctx, SOCKET_PIPE))\
	_(1, "<>&", push_redirection(ctx, REDIRECT_INPUT_OUTPUT_TO_FD))\
	_(0, "&>>", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_APPEND))\
	_(0, "&>&", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_TO_FD))\
	_(0, "&>|", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_CLOBBER))\
	_(1, "()", push_function_mark(ctx))\
	_(0, "((", push_shell_io(ctx, ARITHMETIC_SUBSHELL, RRB_QUOTE_MODE))\
	_(1, ";;", push_command_terminal(ctx, DOUBLE_SEMICOLON))\
	_(0, "<(", push_shell_io(ctx, PROCESS_SUBSTITUTION_OUTPUT, NORMAL_MODE))\
	_(1, "<<", push_redirection(ctx, HERE_DOCUMENT))\
	_(1, "<>", push_redirection(ctx, REDIRECT_INPUT_OUTPUT))\
	_(1, "<&", push_redirection(ctx, REDIRECT_INPUT_TO_FD))\
	_(0, ">(", push_shell_io(ctx, PROCESS_SUBSTITUTION_INPUT, NORMAL_MODE))\
	_(1, ">>", push_redirection(ctx, REDIRECT_OUTPUT_APPEND))\
	_(1, ">&", push_redirection(ctx, REDIRECT_OUTPUT_TO_FD))\
	_(1, ">|", push_redirection(ctx, REDIRECT_OUTPUT_CLOBBER))\
	_(1, "||", push_command_terminal(ctx, OR))\
	_(0, "|&", push_command_terminal(ctx, PIPE_AMPERSAND))\
	_(1, "&&", push_command_terminal(ctx, AND))\
	_(0, "&|", push_command_terminal(ctx, AMPERSAND_PIPE))\
	_(0, "&>", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR))\
	_(1, "(", push_shell_io(ctx, SUBSHELL, NORMAL_MODE))\
	_(1, ";", push_semicolon(ctx, 0))\
	_(1, "<", push_redirection(ctx, REDIRECT_INPUT))\
	_(1, ">", push_redirection(ctx, REDIRECT_OUTPUT))\
	_(1, "|", push_command_terminal(ctx, PIPE))\
	_(1, "&", push_command_terminal(ctx, AMPERSAND))

#define X(PORTABLE, SYMBOL, ACTION)\
	if (token_len >= sizeof(SYMBOL) - 1 &&\
	    !strncmp(token, SYMBOL, sizeof(SYMBOL) - 1) &&\
	    (PORTABLE || check_extension(SYMBOL, ctx->tokeniser_line_number))) {\
		ACTION;\
		return token_len;\
	}
	LIST_SYMBOLS(X)
#undef X

	push_unquoted(ctx, token, 1);
	return 1;
}


static void
push_text(struct parser_context *ctx, char *text, size_t text_len, enum argument_type type)
{
	struct argument *arg_part;

	if (ctx->mode_stack->mode == HERE_DOCUMENT_MODE) {
		type = QUOTED;
		if (ctx->here_document_stack->first->argument_end->type != type ||
		    ctx->here_document_stack->first->argument_end->line_number != ctx->tokeniser_line_number)
			push_new_argument_part(ctx, type);
		arg_part = ctx->here_document_stack->first->argument_end;

	} else {
		ctx->parser_state->need_right_hand_side = 0;

		if (!ctx->parser_state->current_argument_end ||
		    ctx->parser_state->current_argument_end->type != type ||
		    ctx->parser_state->current_argument_end->line_number != ctx->tokeniser_line_number)
			push_new_argument_part(ctx, type);
		arg_part = ctx->parser_state->current_argument_end;
	}

	arg_part->text = erealloc(arg_part->text, arg_part->length + text_len + 1);
	memcpy(&arg_part->text[arg_part->length], text, text_len);
	arg_part->length += text_len;
	arg_part->text[arg_part->length] = '\0';
}


void
push_quoted(struct parser_context *ctx, char *text, size_t text_len)
{
	push_text(ctx, text, text_len, QUOTED);
}


static size_t
encode_utf8(char *buf, uint32_t value)
{
	size_t i, len;

	if (value <= 0x7F) {
		buf[0] = (char)value;
		return 1;
	}

	if      (value <= 0x000007FFUL) len = 2;
	else if (value <= 0x0000FFFFUL) len = 3;
	else if (value <= 0x001FFFFFUL) len = 4;
	else if (value <= 0x03FFFFFFUL) len = 5;
	else if (value <= 0x7FFFFFFFUL) len = 6;
	else                            len = 7;

	for (i = len - 1; i; i--) {
		buf[len - 1 - i] = (char)(((int)value & 0x3F) | 0x80);
		value >>= 6;
	}

	buf[0] |= (char)(0xFF << (8 - len));

	return len;
}

void
push_escaped(struct parser_context *ctx, char *text, size_t text_len)
{
	uint32_t value;
	size_t r, w, n;
	for (r = w = 0; r < text_len;) {
		if (text[r] == '\\' && r + 1 < text_len) {
			if (text[r + 1] == 'a') {
				text[w++] = '\a';
				r += 2;
			} else if (text[r + 1] == 'b') {
				text[w++] = '\b';
				r += 2;
			} else if (text[r + 1] == 'e' || text[r + 1] == 'E') {
				text[w++] = '\033';
				r += 2;
			} else if (text[r + 1] == 'f') {
				text[w++] = '\f';
				r += 2;
			} else if (text[r + 1] == 'n') {
				text[w++] = '\n';
				r += 2;
			} else if (text[r + 1] == 'r') {
				text[w++] = '\r';
				r += 2;
			} else if (text[r + 1] == 't') {
				text[w++] = '\t';
				r += 2;
			} else if (text[r + 1] == 'v') {
				text[w++] = '\v';
				r += 2;
			} else if (text[r + 1] == '\\') {
				text[w++] = '\\';
				r += 2;
			} else if (text[r + 1] == '\'') {
				text[w++] = '\'';
				r += 2;
			} else if (text[r + 1] == '"') {
				text[w++] = '\"';
				r += 2;
			} else if (text[r + 1] == '?') {
				text[w++] = '?';
				r += 2;
			} else if ('0' <= text[r + 1] && text[r + 1] <= '7') {
				value = 0;
				for (r += 1, n = 0; n < 3 && '0' <= text[r + 1] && text[r + 1] <= '7'; r += 1, n += 1) {
					if ((text[r] & 15) > 255 - (int)value)
						break;
					value *= 8;
					value |= (uint32_t)(text[r] & 15);
				}
				if (value) {
					text[w++] = (char)value;
				} else {
					weprintf("ignoring NUL byte result from $''-expression at line %zu\n",
					          ctx->tokeniser_line_number);
				}
			} else if (text[r + 1] == 'x' && text_len - r >= 3 && isxdigit(text[r + 2])) {
				value = 0;
				for (r += 2, n = 0; n < 2 && isxdigit(text[r]); r += 1, n += 1) {
					value *= 16;
					value |= (uint32_t)((text[r] > '9' ? 9 : 0) + (text[r] & 15));
				}
				if (value) {
					text[w++] = (char)value;
				} else {
					weprintf("ignoring NUL byte result from $''-expression at line %zu\n",
					          ctx->tokeniser_line_number);
				}
			} else if (text[r + 1] == 'u' && text_len - r >= 3 && isxdigit(text[r + 2])) {
				value = 0;
				for (r += 2, n = 0; n < 4 && isxdigit(text[r]); r += 1, n += 1) {
					value *= 16;
					value |= (uint32_t)((text[r] > '9' ? 9 : 0) + (text[r] & 15));
				}
				if (value) {
					w += encode_utf8(&text[w], value);
				} else {
					weprintf("ignoring NUL byte result from $''-expression at line %zu\n",
					          ctx->tokeniser_line_number);
				}
			} else if (text[r + 1] == 'U') {
				value = 0;
				for (r += 2, n = 0; n < 8 && isxdigit(text[r]); r += 1, n += 1) {
					value *= 16;
					value |= (uint32_t)((text[r] > '9' ? 9 : 0) + (text[r] & 15));
				}
				if (value) {
					w += encode_utf8(&text[w], value);
				} else {
					weprintf("ignoring NUL byte result from $''-expression at line %zu\n",
					          ctx->tokeniser_line_number);
				}
			} else if (text[r + 1] == 'c' && text_len - r >= 3) {
				if (text[r + 2] & (' ' - 1)) {
					text[w++] = (char)(text[r + 2] & (' ' - 1));
				} else {
					weprintf("ignoring NUL byte result from $''-expression at line %zu\n",
					          ctx->tokeniser_line_number);
				}
				r += 3;
			} else {
				text[w++] = text[r++];
			}
		} else {
			text[w++] = text[r++];
		}
	}
	push_text(ctx, text, w, QUOTED);
}


void
push_unquoted(struct parser_context *ctx, char *text, size_t text_len)
{
	push_text(ctx, text, text_len, UNQUOTED);
}


void
push_enter(struct parser_context *ctx, enum argument_type type)
{
	struct parser_state *new_state;

	if (ctx->mode_stack->mode != HERE_DOCUMENT_MODE)
		ctx->parser_state->need_right_hand_side = 0;

	push_new_argument_part(ctx, type);

	new_state = ecalloc(1, sizeof(*new_state));
	new_state->parent = ctx->parser_state;
	ctx->parser_state->current_argument_end->child = new_state;
	ctx->parser_state = new_state;
}


void
push_leave(struct parser_context *ctx)
{
	struct parser_context subctx;
	struct argument *argument;
	char *code;
	size_t code_length;
	size_t parsed_length;
	size_t arg_i;

	if (ctx->mode_stack->mode == NORMAL_MODE) {
		push_semicolon(ctx, 1);

	} else if (ctx->mode_stack->mode == BQ_QUOTE_MODE) {
		initialise_parser_context(&subctx, 1, 1);
		subctx.do_not_run = 1;
		subctx.end_of_file_reached = 1;
		code = NULL;
		code_length = 0;
		for (arg_i = 0; arg_i < ctx->parser_state->narguments; arg_i++) {
			argument = ctx->parser_state->arguments[arg_i];
			code = erealloc(code, code_length + argument->length);
			memcpy(&code[code_length], argument->text, argument->length);
			code_length += argument->length;
		}
		code = erealloc(code, code_length + 1);
		code[code_length] = '\0';
		parsed_length = parse_preparsed(&subctx, code, code_length);
		if (parsed_length < code_length || subctx.premature_end_of_file) {
			eprintf("premature end of file backquote expression at line %zu\n",
			        ctx->parser_state->parent->current_argument_end->line_number);
		}
		free(code);
		free(subctx.here_document_stack);
		free(subctx.interpreter_state);
		ctx->parser_state->parent->current_argument_end->child = subctx.parser_state;

	} else {
		/* In quote modes we want everything in a dummy command
		 * to simplify the implementation of the interpreter.
		 * The command termination used here doesn't matter,
		 * neither does the line nummer (for it), the interpreter
		 * will only look at the argument list. */
		push_command_terminal(ctx, NEWLINE);
	}

	ctx->parser_state = ctx->parser_state->parent;
}
