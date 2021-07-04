/* See LICENSE file for copyright and license details. */
#include <libsimple.h>
#include <libsimple-arg.h>
#include "config.h"

USAGE("");

enum argument_type {
	VERBATIM,
	ESCAPED,
	SPECIAL,
	FUNCTION_MARK,
	SUBSHELL_INPUT, /* >(...) */
	SUBSHELL_OUTPUT, /* <(...) */
	SUBSHELL_INPUT_OUTPUT, /* <>(...) ## create socket for both input and output of subshell */
	SUBSHELL_SUBSTITUTION,
	SUBSHELL, /* (...) or ((...)) ## if non-first argument: format shell code into a string (can be used for a clean subshell) */
	REDIRECTION /* at beginning of argument, use next redirection and use reminder of argument as right-hand side */
};

enum redirection_type {
	REDIRECT_INPUT,
	REDIRECT_INPUT_TO_FD,
	REDIRECT_OUTPUT,
	REDIRECT_OUTPUT_APPEND,
	REDIRECT_OUTPUT_CLOBBERING,
	REDIRECT_OUTPUT_TO_FD,
	REDIRECT_INPUT_OUTPUT,
	REDIRECT_INPUT_OUTPUT_TO_FD,
	HERE_STRING,
	HERE_DOCUMENT,
	HERE_DOCUMENT_INDENTED
};

enum command_terminal {
	DOUBLE_SEMICOLON,
	SEMICOLON,
	AMPERSAND,
	SOCKET_PIPE,
	PIPE,
	PIPE_AMPERSAND,
	AND,
	OR
};

enum shell_terminator {
	END_OF_FILE,
	ROUND,
	ROUND_ROUND,
	SQUARE,
	BACKTICK,
};

struct parser_state;

struct argument {
	enum argument_type type;
	union {
		struct { /* VERBATIM, ESCAPED */
			char *text;
			size_t length;
		};
		char symbol; /* SPECIAL */
		struct parser_state *root; /* SUBSHELL, SUBSHELL_* */
	}; /* none for FUNCTION_MARK, REDIRECTION */
	struct argument *next_part;
};

struct redirection {
	enum redirection_type type;
	struct argument *left_hand_side;
};

struct command {
	enum command_terminal terminal;
	struct argument **arguments;
	size_t narguments;
	struct redirection **redirections;
	size_t nredirections;
};

struct parser_state {
	struct parser_state *parent;
	struct command **commands;
	size_t ncommands;
	struct argument **arguments;
	size_t narguments;
	struct redirection **redirections;
	size_t nredirections;
	struct argument *current_argument;
	struct argument *current_argument_end;
	enum shell_terminator exit_on;
	char at_dollar;
	char is_expr_shell;
	char need_right_hand_side;
};

struct here_document {
	struct redirection *redirection;
	struct argument *argument;
	struct here_document *next;
};

static size_t line_number = 1;
static int tty_input = 0;

static struct parser_state *state;
static struct here_document *here_documents_first = NULL;
static struct here_document **here_documents_next = &here_documents_first;

static void flush_dollar(void);
static void verbatim(const char *text, size_t text_length, int from_quote);

static void
whitespace(int strict)
{
	flush_dollar();

	if (state->need_right_hand_side) {
		if (strict)
			eprintf("premature end of command\n");
		return;
	}

	if (state->current_argument) {
		state->arguments = erealloc(state->arguments, (state->narguments + 1) * sizeof(*state->arguments));
		state->arguments[state->narguments++] = state->current_argument;
		state->current_argument = NULL;
		state->current_argument_end = NULL;
	}
}

static void
terminate_command(enum command_terminal terminal)
{
	whitespace(1);

	state->commands = erealloc(state->commands, (state->ncommands + 1) * sizeof(*state->commands));
	state->commands[state->ncommands] = ecalloc(1, sizeof(**state->commands));
	state->commands[state->ncommands]->terminal = terminal;
	state->commands[state->ncommands]->arguments = state->arguments;
	state->commands[state->ncommands]->narguments = state->narguments;
	state->commands[state->ncommands]->redirections = state->redirections;
	state->commands[state->ncommands]->nredirections = state->nredirections;
	state->ncommands += 1;
	state->arguments = NULL;
	state->narguments = 0;
	state->redirections = NULL;
	state->nredirections = 0;

	if (!state->parent) {
		if (terminal == DOUBLE_SEMICOLON || terminal == SEMICOLON || terminal == AMPERSAND) {
			/* TODO unless in a special construct such as while, case, for, if, or {, run and clear
			 *      also require that any here-document is specified (count them and run when given)
			 */
		}
	}
}

static void
semicolon(int maybe)
{
	if (!maybe || state->narguments)
		terminate_command(SEMICOLON);
}

static void
end_subshell(void)
{
	semicolon(1);
	/* TODO validate subshell content */
	state = state->parent;
}

static void
add_redirection(enum redirection_type type)
{
	state->redirections = erealloc(state->redirections, (state->nredirections + 1) * sizeof(*state->redirections));
	state->redirections[state->nredirections] = ecalloc(1, sizeof(**state->redirections));
	state->redirections[state->nredirections]->type = type;
	if (state->current_argument) {
		if (state->current_argument->type == REDIRECTION) {
			whitespace(1);
		} else {
			state->redirections[state->nredirections]->left_hand_side = state->current_argument;
			state->current_argument = NULL;
			state->current_argument_end = NULL;
		}
	}
	state->current_argument_end = state->current_argument = calloc(1, sizeof(*state->current_argument));
	state->current_argument_end->type = REDIRECTION;
	if (type == HERE_DOCUMENT || type == HERE_DOCUMENT_INDENTED) {
		*here_documents_next = emalloc(sizeof(**here_documents_next));
		(*here_documents_next)->redirection = state->redirections[state->nredirections];
		(*here_documents_next)->argument = state->current_argument;
		(*here_documents_next)->next = NULL;
		here_documents_next = &(*here_documents_next)->next;
	}
	state->nredirections += 1;
	state->need_right_hand_side = 1;
}

static void
add_shell_io(enum argument_type type, enum shell_terminator exit_on)
{
	struct parser_state *new_state;

	state->need_right_hand_side = 0;

	if (!state->current_argument_end)
		state->current_argument_end = state->current_argument = ecalloc(1, sizeof(struct argument));
	else
		state->current_argument_end = state->current_argument_end->next_part = ecalloc(1, sizeof(struct argument));

	new_state = ecalloc(1, sizeof(*new_state));
	new_state->parent = state;
	new_state->exit_on = exit_on;

	state->current_argument_end->type = type;
	state->current_argument_end->root = state;

	state = new_state;
}

static void
add_function_mark(void)
{
	whitespace(1);
	if (!state->current_argument_end)
		state->current_argument_end = state->current_argument = ecalloc(1, sizeof(struct argument));
	else
		state->current_argument_end = state->current_argument_end->next_part = ecalloc(1, sizeof(struct argument));
	state->current_argument_end->type = FUNCTION_MARK;
	whitespace(1);
}

static void
parse_symbol(const char *token, size_t token_length)
{
	struct parser_state *old_state = state;

	while (token_length) {
		if (state->at_dollar) {
			state->at_dollar = 0;
			if (token_length >= 2 && token[0] == '(' && token[1] == '(') {
				add_shell_io(SUBSHELL_SUBSTITUTION, ROUND_ROUND);
				state->is_expr_shell = 1;
				token = &token[2];
				token_length -= 2;
			} else if (token_length >= 1 && token[0] == '(') {
				add_shell_io(SUBSHELL_SUBSTITUTION, ROUND);
				token = &token[1];
				token_length -= 1;
			} else if (token_length >= 1 && token[0] == '[') {
				add_shell_io(SUBSHELL_SUBSTITUTION, SQUARE);
				state->is_expr_shell = 1;
				token = &token[1];
				token_length -= 1;
			} else if (token_length >= 1 && token[0] == '{') { /* TODO */
				token = &token[1];
				token_length -= 1;
			} else {
				state->at_dollar = 1;
				flush_dollar();
				continue;
			}
		}

		if (token_length >= 3 && token[0] == '<' && token[1] == '<' && token[2] == '<') {
			add_redirection(HERE_STRING);
			token = &token[3];
			token_length -= 3;

		} else if (token_length >= 3 && token[0] == '<' && token[1] == '<' && token[2] == '-') {
			add_redirection(HERE_DOCUMENT_INDENTED);
			token = &token[3];
			token_length -= 3;

		} else if (token_length >= 3 && token[0] == '<' && token[1] == '>' && token[2] == '(') {
			add_shell_io(SUBSHELL_INPUT_OUTPUT, ROUND);
			token = &token[3];
			token_length -= 3;

		} else if (token_length >= 3 && token[0] == '<' && token[1] == '>' && token[2] == '|') {
			terminate_command(SOCKET_PIPE);
			token = &token[3];
			token_length -= 3;

		} else if (token_length >= 3 && token[0] == '<' && token[1] == '>' && token[2] == '&') {
			add_redirection(REDIRECT_INPUT_OUTPUT_TO_FD);
			token = &token[3];
			token_length -= 3;

		} else if (token_length >= 2 && token[0] == ')' && token[1] == ')') {
			if (state->exit_on == ROUND_ROUND)
				end_subshell();
			else
				eprintf("stray )) at line %zu\n", line_number);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '(' && token[1] == ')') {
			add_function_mark();
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '(' && token[1] == '(') {
			add_shell_io(SUBSHELL, ROUND_ROUND);
			state->is_expr_shell = 1;
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == ';' && token[1] == ';') {
			terminate_command(DOUBLE_SEMICOLON);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '<' && token[1] == '(') {
			add_shell_io(SUBSHELL_OUTPUT, ROUND);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '<' && token[1] == '<') {
			add_redirection(HERE_DOCUMENT);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '<' && token[1] == '>') {
			add_redirection(REDIRECT_INPUT_OUTPUT);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '<' && token[1] == '&') {
			add_redirection(REDIRECT_INPUT_TO_FD);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '>' && token[1] == '(') {
			add_shell_io(SUBSHELL_INPUT, ROUND);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '>' && token[1] == '>') {
			add_redirection(REDIRECT_OUTPUT_APPEND);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '>' && token[1] == '&') {
			add_redirection(REDIRECT_OUTPUT_TO_FD);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '>' && token[1] == '|') {
			add_redirection(REDIRECT_OUTPUT_CLOBBERING);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '|' && token[1] == '&') {
			terminate_command(PIPE_AMPERSAND);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '|' && token[1] == '|') {
			terminate_command(OR);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 2 && token[0] == '&' && token[1] == '&') {
			terminate_command(AND);
			token = &token[2];
			token_length -= 2;

		} else if (token_length >= 1 && token[0] == ')') {
			if (state->exit_on == ROUND)
				end_subshell();
			else
				eprintf("stray ) at line %zu\n", line_number);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == ']') {
			if (state->exit_on == SQUARE)
				end_subshell();
			else
				verbatim(token, 1, 0);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == '(') {
			add_shell_io(SUBSHELL, ROUND);
			state->is_expr_shell = old_state->is_expr_shell;
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == ';') {
			semicolon(0);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == '<') {
			add_redirection(REDIRECT_INPUT);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == '>') {
			add_redirection(REDIRECT_OUTPUT);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == '|') {
			terminate_command(PIPE);
			token = &token[1];
			token_length -= 1;

		} else if (token_length >= 1 && token[0] == '&') {
			terminate_command(AMPERSAND);
			token = &token[1];
			token_length -= 1;

		} else {
			verbatim(token, 1, 0);
			token = &token[1];
			token_length -= 1;
		}
	}
}

static void
symbol(char *token, size_t token_length, size_t escaped_newlines)
{
	size_t new_length, r, w;
	if (escaped_newlines) {
		r = w = 0;
		new_length = token_length - 2 * escaped_newlines;
		while (escaped_newlines--) {
			if (token[r] == '\\')
				r += 2;
			else
				token[w++] = token[r++];
		}
		memcpy(&token[w], &token[r], token_length - r);
		token_length = new_length;
	}
	parse_symbol(token, token_length);
}

static void
backtick(void)
{
	flush_dollar();
	if (state->exit_on == BACKTICK)
		end_subshell();
	else
		add_shell_io(SUBSHELL_SUBSTITUTION, BACKTICK);
}

static void
double_quote(void)
{
	flush_dollar();
	/* TODO */
}

static void
verbatim(const char *text, size_t text_length, int from_quote)
{
	struct argument *argend;

	state->need_right_hand_side = 0;

	if (from_quote && state->at_dollar) {
		state->at_dollar = 0;
		if (!state->current_argument_end)
			state->current_argument_end = state->current_argument = ecalloc(1, sizeof(struct argument));
		else
			state->current_argument_end = state->current_argument_end->next_part = ecalloc(1, sizeof(struct argument));
		state->current_argument_end->type = ESCAPED;
	} else {
		flush_dollar();
		if (!state->current_argument_end) {
			state->current_argument_end = state->current_argument = ecalloc(1, sizeof(struct argument));
			state->current_argument_end->type = VERBATIM;
		} else if (state->current_argument_end->type != VERBATIM) {
			state->current_argument_end = state->current_argument_end->next_part = ecalloc(1, sizeof(struct argument));
			state->current_argument_end->type = VERBATIM;
		}
	}

	argend = state->current_argument_end;

	argend->text = erealloc(argend->text, argend->length + text_length + 1);
	memcpy(&argend->text[argend->length], text, text_length);
	argend->length += text_length;
	argend->text[argend->length] = '\0';
}

static void
flush_dollar(void)
{
	if (state->at_dollar) {
		state->at_dollar = 0;
		verbatim("$", 1, 0);
	}
}

static void
append_special(char symbol)
{
	state->need_right_hand_side = 0;

	if (!state->current_argument_end)
		state->current_argument_end = state->current_argument = ecalloc(1, sizeof(struct argument));
	else
		state->current_argument_end = state->current_argument_end->next_part = ecalloc(1, sizeof(struct argument));

	state->current_argument_end->type = SPECIAL;
	state->current_argument_end->symbol = symbol;
}

static void
unverbatim(const char *text, size_t text_length)
{
	size_t verbatim_length;

	/* TODO handle state->dollar */

	while (text_length) {
		for (verbatim_length = 0; verbatim_length < text_length; verbatim_length++)
			if (*text == '*' || *text == '?' || *text == '[' || *text == ']' ||
			    *text == ',' || *text == '.' || *text == '{' || *text == '}' ||
			    *text == '~' || *text == '!' || *text == '=')
				break;
		if (verbatim_length) {
			verbatim(text, verbatim_length, 0);
			text = &text[verbatim_length];
			text_length -= verbatim_length;
		} else {
			append_special(*text);
			text = &text[1];
			text_length -= 1;
		}
	}
}

static void
dollar(void)
{
	/* TODO forbid $ if giving argument to here-document */
	if (state->at_dollar)
		unverbatim("$", 1);
	else
		state->at_dollar = 1;
}

static int
end_of_file(void)
{
	semicolon(1);
	return !(state->parent || state->ncommands);
}

static size_t
parse(char *code, size_t code_len, int end_of_file_reached)
{
#define IS_SYMBOL(C)\
	((!state->is_expr_shell && (\
	  (C) == '|' || (C) == '&' || (C) == ';' || \
	  (C) == '<' || (C) == '>' || (C) == '-')) || \
	 (C) == '{' || (C) == '}' || \
	 (C) == '(' || (C) == ')' || \
	 (C) == '[' || (C) == ']')

	static int she_is_comment = 1;
	static int in_comment = 0;
	static int at_line_beginning = 1;

	size_t read_bytes = 0;
	size_t token_len;
	size_t new_lines;

	for (; read_bytes < code_len; read_bytes += token_len, code = &code[token_len]) {
		if (at_line_beginning) {
			if (here_documents_first) {
				/* TODO read until terminator, remove indentation if <<- and then parse in "-mode but accept " */
			}
			at_line_beginning = 0;
		}

		if (in_comment) {
			if (*code == '\n') {
				in_comment = 0;
			} else {
				token_len = 1;
				continue;
			}
		}

		if (*code == '\0') {
			if (!tty_input)
				weprintf("ignoring NUL byte at line %zu\n", line_number);

		} else if (*code == '\n') {
			line_number += 1;
			she_is_comment = 1;
			whitespace(0);
			semicolon(1);
			token_len = 1;
			at_line_beginning = 1;

		} else if (isspace(*code)) {
			she_is_comment = 1;
			whitespace(0);
			for (token_len = 1; token_len < code_len - read_bytes; token_len++)
				if (!isspace(code[token_len]) || code[token_len] == '\n')
					break;

		} else if (*code == '#' && she_is_comment) {
			in_comment = 1;
			token_len = 1;

		} else if (IS_SYMBOL(*code)) {
			she_is_comment = 1;
			new_lines = 0;
			for (token_len = 1; token_len < code_len - read_bytes; token_len++) {
				if (code[token_len] == '\\' && token_len + 1 < code_len - read_bytes && code[token_len] == '\n') {
					new_lines += 1;
				} else if (!IS_SYMBOL(code[token_len])) {
					symbol(code, token_len, new_lines);
					line_number += new_lines;
					goto next;
				}
			}
			if (end_of_file_reached) {
				symbol(code, token_len, new_lines);
				line_number += new_lines;
			} else {
				break;
			}

		} else if (*code == '\\') {
			she_is_comment = 0;
			if (code_len - read_bytes < 2)
				break;
			token_len = 2;
			if (code[1] == '\n')
				line_number += 1;
			else
				verbatim(&code[1], 1, 0);

		} else if (*code == '$') {
			she_is_comment = 0;
			dollar();
			token_len = 1;

		} else if (*code == '`') {
			she_is_comment = 1;
			backtick();
			token_len = 1;

		} else if (*code == '"') {
			she_is_comment = 0;
			double_quote();

		} else if (*code == '\'') {
			she_is_comment = 0;
			new_lines = 0;
			for (token_len = 1; token_len < code_len - read_bytes; token_len++) {
				if (code[token_len] == '\'') {
					token_len += 1;
					if (!state->at_dollar || code[token_len - 2] != '\\') {
						verbatim(&code[1], token_len - 2, 1);
						line_number += new_lines;
						goto next;
					}
				} else if (code[token_len] == '\n') {
					new_lines += 1;
				}
			}
			break;

		} else {
			she_is_comment = 0;
			for (token_len = 1; token_len < code_len - read_bytes; token_len++)
				if (isspace(*code) || IS_SYMBOL(*code) || *code == '\\' ||
				    *code == '$' || *code == '`' || *code == '"' || *code == '\'')
					break;
			unverbatim(code, token_len);
		}

	next:;
	}

	return read_bytes;

#undef IS_SYMBOL
}

int
main(int argc, char *argv[])
{
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t buffer_head = 0;
	size_t buffer_tail = 0;
	ssize_t r;
	size_t n;

	ARGBEGIN {
	default:
		usage();
	} ARGEND;

	if (argc)
		usage();

	tty_input = isatty(STDIN_FILENO);
	if (tty_input)
		weprintf("apsh is currently not implemented to be interactive\n");

	state = ecalloc(1, sizeof(*state));

	for (;;) {
		if (buffer_size - buffer_head < PARSE_RINGBUFFER_MIN_AVAILABLE) {
			if (buffer_tail && buffer_head - buffer_tail <= buffer_tail) {
				memcpy(&buffer[0], &buffer[buffer_tail], buffer_head - buffer_tail);
				buffer_head -= buffer_tail;
				buffer_tail = 0;
			}
			if (buffer_size - buffer_head < PARSE_RINGBUFFER_MIN_AVAILABLE)
				buffer = erealloc(buffer, buffer_size += PARSE_RINGBUFFER_INCREASE_SIZE);
		}

		r = read(STDIN_FILENO, &buffer[buffer_head], buffer_size - buffer_head);
		if (r <= 0) {
			if (!r)
				break;
			eprintf("read <stdin>:");
		}
		n = (size_t)r;

		buffer_head += n;
		buffer_tail += parse(&buffer[buffer_tail], buffer_head - buffer_tail, 0);
	}

	buffer_tail += parse(&buffer[buffer_tail], buffer_head - buffer_tail, 1);
	if (buffer_tail != buffer_head || !end_of_file())
		eprintf("premature end of file reached\n");

	free(buffer);
	return 0;
}
