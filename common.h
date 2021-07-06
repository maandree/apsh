/* See LICENSE file for copyright and license details. */
#include <libsimple.h>
#include <libsimple-arg.h>
#include "config.h"


enum argument_type {
	/* .text and .length */
	QUOTED, /* \ or '…' or $'…' */
	UNQUOTED, /* normal */
	/* .child */
	QUOTE_EXPRESSION, /* "…" */
	BACKQUOTE_EXPRESSION, /* `…` */
	ARITHMETIC_EXPRESSION, /* $((…)) */
	VARIABLE_SUBSTITUTION, /* ${…} */
	SUBSHELL_SUBSTITUTION, /* $(…) */
	PROCESS_SUBSTITUTION_INPUT, /* >(…) */
	PROCESS_SUBSTITUTION_OUTPUT, /* <(…) */
	PROCESS_SUBSTITUTION_INPUT_OUTPUT, /* <>(…) */
	SUBSHELL, /* (…) ## if non-first argument: format shell code into a string (can be used for a clean subshell) */
	ARITHMETIC_SUBSHELL, /* ((…)) ## if non-first argument: format shell code into a string */
	/* (none) */
	REDIRECTION, /* at beginning of argument, use next redirection and use reminder of argument as right-hand side */
	FUNCTION_MARK /* () */
};

enum redirection_type {
	REDIRECT_INPUT,
	REDIRECT_INPUT_TO_FD,
	REDIRECT_OUTPUT,
	REDIRECT_OUTPUT_APPEND,
	REDIRECT_OUTPUT_CLOBBER,
	REDIRECT_OUTPUT_TO_FD,
	REDIRECT_OUTPUT_AND_STDERR,
	REDIRECT_OUTPUT_AND_STDERR_APPEND,
	REDIRECT_OUTPUT_AND_STDERR_CLOBBER,
	REDIRECT_OUTPUT_AND_STDERR_TO_FD,
	REDIRECT_INPUT_OUTPUT,
	REDIRECT_INPUT_OUTPUT_TO_FD,
	HERE_STRING,
	HERE_DOCUMENT,
	HERE_DOCUMENT_INDENTED
};

enum tokeniser_mode {
	NORMAL_MODE,
	COMMENT_MODE,
	BQ_QUOTE_MODE,
	DQ_QUOTE_MODE,
	RRB_QUOTE_MODE,
	RB_QUOTE_MODE,
	SB_QUOTE_MODE,
	CB_QUOTE_MODE,
	HERE_DOCUMENT_MODE
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

struct parser_state;

struct argument {
	enum argument_type type;
	union {
		struct {
			char *text;
			size_t length;
		};
		struct parser_state *child;
	};
	size_t line_number;
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
	char need_right_hand_side;
};

struct here_document {
	struct redirection *redirection;
	struct argument *argument;
	struct here_document *next;
};

struct mode_stack {
	enum tokeniser_mode mode;
	int she_is_comment;
	struct mode_stack *previous;
};

struct parser_context {
	int tty_input;
	int end_of_file_reached;
	int premature_end_of_file;
	size_t preparser_offset;
	size_t preparser_line_number;
	size_t line_continuations;
	size_t tokeniser_line_number;
	struct mode_stack *mode_stack;
	struct parser_state *parser_state;
	struct here_document *here_documents_first;
	struct here_document **here_documents_next;
};


/* apsh.c */
void initialise_parser_context(struct parser_context *ctx);

/* preparser.c */
size_t parse(struct parser_context *ctx, char *code, size_t code_len, size_t *nremovedp);

/* tokeniser.c */
void push_mode(struct parser_context *ctx, enum tokeniser_mode mode);
void pop_mode(struct parser_context *ctx);
size_t parse_preparsed(struct parser_context *ctx, char *code, size_t code_len);

/* parser.c */
void push_end_of_file(struct parser_context *ctx);
void push_whitespace(struct parser_context *ctx, int strict);
void push_semicolon(struct parser_context *ctx, int maybe);
size_t push_symbol(struct parser_context *ctx, char *token, size_t token_len);
void push_quoted(struct parser_context *ctx, char *text, size_t text_len);
void push_escaped(struct parser_context *ctx, char *text, size_t text_len);
void push_unquoted(struct parser_context *ctx, char *text, size_t text_len);
void push_enter(struct parser_context *ctx, enum argument_type type);
void push_leave(struct parser_context *ctx);
