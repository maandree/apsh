/* See LICENSE file for copyright and license details. */
#include <libsimple.h>
#include <libsimple-arg.h>
#include "config.h"


#if defined(__GNUC__)
# define CONST_FUNC __attribute__((__const__))
# define PURE_FUNC  __attribute__((__pure__))
#else
# define CONST_FUNC
# define PURE_FUNC
#endif


#define BUILTIN_USAGE(FUNCTION_NAME, SYNOPSIS)\
	BUILTIN_NUSAGE(1, FUNCTION_NAME, SYNOPSIS)

#define BUILTIN_NUSAGE(STATUS, FUNCTION_NAME, SYNOPSIS)\
	static void\
	FUNCTION_NAME(void)\
	{\
		const char *syn = SYNOPSIS ? SYNOPSIS : "";\
		fprintf(stderr, "usage: %s%s%s\n", argv0, *syn ? " " : "", syn);\
		exit(STATUS);\
	}


enum argument_type {
	/* .text and .length */
	QUOTED, /* \ or '…' or $'…' */
	UNQUOTED, /* normal */
	VARIABLE, /* used by interpreter, not parser */
	OPERATOR, /* used by interpreter for ${}, not parser */
	/* .child, but changed to .command by interpreter */
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
	/* .command */
	COMMAND, /* used by interpreter, not parser */
	/* (none) */
	REDIRECTION, /* at beginning of argument, use next redirection and use reminder of argument as right-hand side */
	FUNCTION_MARK /* () */
};

enum nesting_type {
	MAIN_BODY,
	CODE_ROOT,
	TEXT_ROOT,
	VARIABLE_SUBSTITUTION_BRACKET,
	CURLY_NESTING,
	IF_STATEMENT,
	IF_CONDITIONAL,
	IF_CLAUSE,
	ELSE_CLAUSE,
	UNTIL_STATEMENT,
	WHILE_STATEMENT,
	REPEAT_CONDITIONAL,
	DO_CLAUSE,
	FOR_STATEMENT
};

enum redirection_type {
	REDIRECT_INPUT,
	REDIRECT_INPUT_TO_FD, /* but close if right-hand side is "-" */
	REDIRECT_OUTPUT,
	REDIRECT_OUTPUT_APPEND,
	REDIRECT_OUTPUT_CLOBBER,
	REDIRECT_OUTPUT_TO_FD, /* ditto */
	REDIRECT_OUTPUT_AND_STDERR,
	REDIRECT_OUTPUT_AND_STDERR_APPEND,
	REDIRECT_OUTPUT_AND_STDERR_CLOBBER,
	REDIRECT_OUTPUT_AND_STDERR_TO_FD, /* ditto */
	REDIRECT_INPUT_OUTPUT,
	REDIRECT_INPUT_OUTPUT_TO_FD, /* ditto */
	HERE_STRING,
	HERE_DOCUMENT, /* eliminated during parse */
	HERE_DOCUMENT_INDENTED /* eliminated during parse */
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
	HERE_DOCUMENT_MODE_INITIALISATION,
	HERE_DOCUMENT_MODE
};

enum command_terminal {
	DOUBLE_SEMICOLON,
	SEMICOLON,
	NEWLINE,
	AMPERSAND,
	SOCKET_PIPE,
	PIPE,
	PIPE_AMPERSAND,
	AMPERSAND_PIPE, /* synonym for |& to match &> */
	AND,
	OR
};

enum interpreter_requirement {
	NEED_COMMAND = 0,
	NEED_COMMAND_END,
	NO_REQUIREMENT,
	NEED_FUNCTION_BODY,
	NEED_VARIABLE_NAME,
	NEED_IN_OR_DO,
	NEED_DO,
	NEED_VALUE,
	NEED_PREFIX_OR_VARIABLE_NAME,
	NEED_INDEX_OR_OPERATOR_OR_END,
	NEED_INDEX_OR_SUFFIX_OR_END,
	NEED_INDEX_OR_END,
	NEED_OPERATOR_OR_END,
	NEED_AT_OPERAND,
	NEED_TEXT_OR_SLASH,
	NEED_TEXT_OR_COLON,
	NEED_END
};

struct parser_state;
struct interpreter_state;

struct argument {
	enum argument_type type;
	union {
		struct {
			char *text;
			size_t length;
		};
		struct parser_state *child;
		struct interpreter_state *command;
	};
	/* (TODO) need to be able to track locations of functions, dots, evals, and maybe aliases,
	 *        as well as filenames, so a more complex tracking method is required, basically
	 *        a reversed tree (stack with reference counted nodes) with filename and linenumber
	 *        nodes, with type annotation; however for memory efficiency, .line_number shall
	 *        still be used for the leaves */
	size_t line_number;
	struct argument *next_part;
};

struct redirection {
	enum redirection_type type;
	struct argument *left_hand_side;
	struct argument *right_hand_side; /* set by interpreter, not parser */
};

struct command {
	enum command_terminal terminal;
	char have_bang; /* set by interpreter */
	size_t terminal_line_number; /* (TODO) same idea as in `struct argument` */
	struct argument **arguments;
	size_t narguments;
	struct redirection **redirections;
	size_t nredirections;
	size_t redirections_offset; /* used by interpreter */
};

struct parser_state {
	struct parser_state *parent;
	struct command **commands; /* in text nodes, all text will be in at most one argument in a single dummy command */
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
	struct argument *argument_end;
	char *terminator;
	size_t terminator_length;
	struct here_document *next;
};

struct mode_stack {
	enum tokeniser_mode mode;
	int she_is_comment;
	struct mode_stack *previous;
};

struct here_document_stack {
	char indented;
	char verbatim;
	char interpret_when_empty;
	size_t line_offset;
	struct here_document *first;
	struct here_document **next;
	struct here_document_stack *previous;
};

struct interpreter_state {
	enum nesting_type dealing_with;
	enum interpreter_requirement requirement;
	char allow_newline;
	char disallow_bang; /* disallow rather than allow, so that default value is 0 */
	char have_bang;
	struct command **commands; /* normally the results are stored here */
	size_t ncommands;
	struct argument **arguments; /* for TEXT_ROOT and VARIABLE_SUBSTITUTION_BRACKET, results are stored here */
	size_t narguments;
	struct redirection **redirections;
	size_t nredirections;
	struct interpreter_state *parent;
};

struct parser_context {
	char tty_input;
	char end_of_file_reached;
	char premature_end_of_file;
	char do_not_run;
	size_t preparser_offset;
	size_t preparser_line_number;
	size_t line_continuations;
	size_t tokeniser_line_number;
	size_t interpreter_offset;
	struct mode_stack *mode_stack;
	struct parser_state *parser_state;
	struct here_document_stack *here_document_stack;
	struct interpreter_state *interpreter_state;
};


/* apsh.c */
extern int login_shell;
extern int posix_mode;
void initialise_parser_context(struct parser_context *ctx, int need_tokeniser, int need_parser);

/* preparser.c */
size_t parse(struct parser_context *ctx, char *code, size_t code_len, size_t *nremovedp);

/* tokeniser.c */
void push_mode(struct parser_context *ctx, enum tokeniser_mode mode);
void pop_mode(struct parser_context *ctx);
int check_extension(const char *token, size_t line_number);
size_t parse_preparsed(struct parser_context *ctx, char *code, size_t code_len);

/* parser.c */
PURE_FUNC const char *get_redirection_token(enum redirection_type type);
void push_end_of_file(struct parser_context *ctx);
void push_whitespace(struct parser_context *ctx, int strict);
void push_semicolon(struct parser_context *ctx, int actually_newline);
size_t push_symbol(struct parser_context *ctx, char *token, size_t token_len);
void push_quoted(struct parser_context *ctx, char *text, size_t text_len);
void push_escaped(struct parser_context *ctx, char *text, size_t text_len);
void push_unquoted(struct parser_context *ctx, char *text, size_t text_len);
void push_enter(struct parser_context *ctx, enum argument_type type);
void push_leave(struct parser_context *ctx);

/* interpreter.c */
void interpret_and_eliminate(struct parser_context *ctx);

/* special_builtins.c */
#define LIST_SPECIAL_BUILTINS(_)\
	_(":", colon_main, CONST_FUNC)

/* regular_builtins.c */
#define LIST_REGULAR_BUILTINS(_)\
	_("true", true_main, CONST_FUNC)\
	_("false", false_main, CONST_FUNC)\
	_("pwd", pwd_main,)
/* "true" and "false" are defined as regular built-in shell utilities
 * (that must be searched before PATH), not as stand-alone utilities,
 * in POSIX (but vice verse in LSB). "pwd" is defined both as regular
 * built-in shell utility and as a stand-alone utility. */

#define X(SH_NAME, C_FUNCTION, C_ATTRIBUTES)\
	C_ATTRIBUTES int C_FUNCTION(int argc, char **argv);
LIST_SPECIAL_BUILTINS(X)
LIST_REGULAR_BUILTINS(X)
#undef X
