/* See LICENSE file for copyright and license details. */
#include "common.h"


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
	new_command->arguments = ctx->parser_state->arguments;
	new_command->narguments = ctx->parser_state->narguments;
	new_command->redirections = ctx->parser_state->redirections;
	new_command->nredirections = ctx->parser_state->nredirections;
	ctx->parser_state->arguments = NULL;
	ctx->parser_state->narguments = 0;
	ctx->parser_state->redirections = NULL;
	ctx->parser_state->nredirections = 0;

	if (!ctx->parser_state->parent) {
		if (terminal == DOUBLE_SEMICOLON || terminal == SEMICOLON || terminal == AMPERSAND) {
			/* TODO unless in a special construct such as while, case, for, if, or {, run and clear
			 *      also require that any here-document is specified (count them and run when given);
			 *      if terminal == AMPERSAND: perform </dev/null first, and reset exist status to 0
			 */
		}
	}
}


void
push_semicolon(struct parser_context *ctx, int maybe)
{
	if (!maybe || ctx->parser_state->narguments)
		push_command_terminal(ctx, SEMICOLON);
}


static void
push_new_argument_part(struct parser_context *ctx, enum argument_type type)
{
	struct argument *new_part;

	new_part = ecalloc(1, sizeof(*new_part));
	new_part->type = type;
	new_part->line_number = ctx->tokeniser_line_number;

	if (ctx->parser_state->current_argument_end) {
		ctx->parser_state->current_argument_end->next_part = new_part;
		ctx->parser_state->current_argument_end = new_part;
	} else {
		ctx->parser_state->current_argument = new_part;
		ctx->parser_state->current_argument_end = new_part;
	}
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
		    type == REDIRECT_OUTPUT_AND_STDERR_TO_FD) {
			push_whitespace(ctx, 1);
		} else {
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
		*ctx->here_documents_next = new_here_document;
		ctx->here_documents_next = &new_here_document->next;
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
	_("<<<", push_redirection(ctx, HERE_STRING))\
	_("<<-", push_redirection(ctx, HERE_DOCUMENT_INDENTED))\
	_("<>(", push_shell_io(ctx, PROCESS_SUBSTITUTION_INPUT_OUTPUT, NORMAL_MODE))\
	_("<>|", push_command_terminal(ctx, SOCKET_PIPE))\
	_("<>&", push_redirection(ctx, REDIRECT_INPUT_OUTPUT_TO_FD))\
	_("&>>", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_APPEND))\
	_("&>&", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_TO_FD))\
	_("&>|", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR_CLOBBER))\
	_("()", push_function_mark(ctx))\
	_("((", push_shell_io(ctx, ARITHMETIC_SUBSHELL, RRB_QUOTE_MODE))\
	_(";;", push_command_terminal(ctx, DOUBLE_SEMICOLON))\
	_("<(", push_shell_io(ctx, PROCESS_SUBSTITUTION_OUTPUT, NORMAL_MODE))\
	_("<<", push_redirection(ctx, HERE_DOCUMENT))\
	_("<>", push_redirection(ctx, REDIRECT_INPUT_OUTPUT))\
	_("<&", push_redirection(ctx, REDIRECT_INPUT_TO_FD))\
	_(">(", push_shell_io(ctx, PROCESS_SUBSTITUTION_INPUT, NORMAL_MODE))\
	_(">>", push_redirection(ctx, REDIRECT_OUTPUT_APPEND))\
	_(">&", push_redirection(ctx, REDIRECT_OUTPUT_TO_FD))\
	_(">|", push_redirection(ctx, REDIRECT_OUTPUT_CLOBBER))\
	_("||", push_command_terminal(ctx, OR))\
	_("|&", push_command_terminal(ctx, PIPE_AMPERSAND))\
	_("&&", push_command_terminal(ctx, AND))\
	_("&|", push_command_terminal(ctx, PIPE_AMPERSAND)) /* synonym for |& to match &> */\
	_("&>", push_redirection(ctx, REDIRECT_OUTPUT_AND_STDERR))\
	_("(", push_shell_io(ctx, SUBSHELL, NORMAL_MODE))\
	_(";", push_semicolon(ctx, 0))\
	_("<", push_redirection(ctx, REDIRECT_INPUT))\
	_(">", push_redirection(ctx, REDIRECT_OUTPUT))\
	_("|", push_command_terminal(ctx, PIPE))\
	_("&", push_command_terminal(ctx, AMPERSAND))

#define X(SYMBOL, ACTION)\
	if (token_len >= sizeof(SYMBOL) - 1 && !strncmp(token, SYMBOL, sizeof(SYMBOL) - 1)) {\
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

	ctx->parser_state->need_right_hand_side = 0;

	if (!ctx->parser_state->current_argument_end ||
	    ctx->parser_state->current_argument_end->type != type ||
	    ctx->parser_state->current_argument_end->line_number != ctx->tokeniser_line_number)
		push_new_argument_part(ctx, type);
	arg_part = ctx->parser_state->current_argument_end;

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


void
push_escaped(struct parser_context *ctx, char *text, size_t text_len)
{
	/* TODO resolve backslashes in text */
	push_text(ctx, text, text_len, QUOTED);
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
	if (ctx->mode_stack->mode == NORMAL_MODE)
		push_semicolon(ctx, 1);
	/* TODO else if (ctx->mode_stack->mode == BQ_QUOTE_MODE), parse content */
	/* TODO validate subshell content */
	ctx->parser_state = ctx->parser_state->parent;
}
