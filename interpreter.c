/* See LICENSE file for copyright and license details. */
#include "common.h"


#define LIST_RESERVED_WORDS(_)\
	_("!",     BANG)\
	_("{",     OPEN_CURLY)\
	_("}",     CLOSE_CURLY)\
	_("case",  CASE) /* (TODO) case patterns requires update to tokeniser */\
	_("do",    DO)\
	_("done",  DONE)\
	_("elif",  ELIF)\
	_("else",  ELSE)\
	_("esac",  ESAC)\
	_("fi",    FI)\
	_("for",   FOR)\
	_("if",    IF)\
	_("in",    IN)\
	_("then",  THEN)\
	_("until", UNTIL)\
	_("while", WHILE)

#define X(S, C) ,C
enum reserved_word {
	NOT_A_RESERVED_WORD = 0
	LIST_RESERVED_WORDS(X)
};
#undef X


PURE_FUNC
static enum reserved_word
get_reserved_word(struct argument *argument)
{
	if (argument->type != UNQUOTED || argument->next_part)
		return NOT_A_RESERVED_WORD;
#define X(S, C)\
	if (argument->length == sizeof(S) - 1 && !strcmp(argument->text, S))\
		return C;
	LIST_RESERVED_WORDS(X)
#undef X
	return NOT_A_RESERVED_WORD;
}


static void
stray_command_terminal(struct command *command)
{
	switch (command->terminal) {
	case DOUBLE_SEMICOLON: eprintf("stray ';;' at line %zu\n",      command->terminal_line_number); return;
	case SEMICOLON:        eprintf("stray ';' at line %zu\n",       command->terminal_line_number); return;
	case NEWLINE:          eprintf("stray <newline> at line %zu\n", command->terminal_line_number); return;
	case AMPERSAND:        eprintf("stray '&' at line %zu\n",       command->terminal_line_number); return;
	case SOCKET_PIPE:      eprintf("stray '<>|' at line %zu\n",     command->terminal_line_number); return;
	case PIPE:             eprintf("stray '|' at line %zu\n",       command->terminal_line_number); return;
	case PIPE_AMPERSAND:   eprintf("stray '|&' at line %zu\n",      command->terminal_line_number); return;
	case AMPERSAND_PIPE:   eprintf("stray '&|' at line %zu\n",      command->terminal_line_number); return;
	case AND:              eprintf("stray '&&' at line %zu\n",      command->terminal_line_number); return;
	case OR:               eprintf("stray '||' at line %zu\n",      command->terminal_line_number); return;
	default:
		abort();
	}
}


static void
stray_reserved_word(struct argument *argument)
{
	eprintf("stray '%s' at line %zu\n", argument->text, argument->line_number);
}


static void
stray_redirection(struct command *command, struct argument *argument)
{
	enum redirection_type type = command->redirections[command->redirections_offset]->type;
	eprintf("stray '%s' at line %zu\n", get_redirection_token(type), argument->line_number);
}


static void
free_text_argument(struct argument **argumentp)
{
	struct argument *argument = *argumentp;
	*argumentp = argument->next_part;
	free(argument->text);
	free(argument);
}


static void
push_interpreted_argument(struct parser_context *ctx, struct argument *argument)
{
	ctx->interpreter_state->arguments = erealloc(ctx->interpreter_state->arguments,
	                                             (ctx->interpreter_state->narguments + 1) *
	                                             sizeof(*ctx->interpreter_state->arguments));
	ctx->interpreter_state->arguments[ctx->interpreter_state->narguments] = argument;
	ctx->interpreter_state->narguments += 1;
}


static void
push_state(struct parser_context *ctx, enum nesting_type dealing_with, size_t line_number)
{
	struct interpreter_state *new_state;
	struct argument *new_argument;
	new_state = ecalloc(1, sizeof(*new_state));
	new_state->parent = ctx->interpreter_state;
	new_state->dealing_with = dealing_with;
	new_argument = calloc(1, sizeof(*new_argument));
	new_argument->type = COMMAND;
	new_argument->command = new_state;
	new_argument->line_number = line_number;
	push_interpreted_argument(ctx, new_argument);
	ctx->interpreter_state = new_state;
}


static void
pop_state(struct parser_context *ctx)
{
	ctx->interpreter_state = ctx->interpreter_state->parent;
}


static void
push_command(struct parser_context *ctx, struct command *command)
{
	free(command->redirections);
	free(command->arguments);
	command->redirections = ctx->interpreter_state->redirections;
	command->nredirections = ctx->interpreter_state->nredirections;
	command->arguments = ctx->interpreter_state->arguments;
	command->narguments = ctx->interpreter_state->narguments;
	command->have_bang = ctx->interpreter_state->have_bang;
	ctx->interpreter_state->redirections = NULL;
	ctx->interpreter_state->nredirections = 0;
	ctx->interpreter_state->arguments = NULL;
	ctx->interpreter_state->narguments = 0;
	ctx->interpreter_state->have_bang = 0;
	ctx->parser_state->commands[ctx->interpreter_offset] = NULL;

	ctx->interpreter_state->commands = erealloc(ctx->interpreter_state->commands,
	                                            (ctx->interpreter_state->ncommands + 1) * 
	                                            sizeof(*ctx->interpreter_state->commands));
	ctx->interpreter_state->commands[ctx->interpreter_state->ncommands] = command;
	ctx->interpreter_state->ncommands += 1;
}


static void
interpret_nested_code(struct argument *argument, enum nesting_type dealing_with, enum interpreter_requirement requirement)
{
	struct parser_state *code = argument->child;
	struct parser_context ctx;

	initialise_parser_context(&ctx, 0, 0);
	ctx.parser_state = code;
	ctx.interpreter_state->dealing_with = dealing_with;
	ctx.interpreter_state->requirement = requirement;

	interpret_and_eliminate(&ctx);

	if (ctx.parser_state->ncommands)
		eprintf("premature end of subexpression at line %zu\n", argument->line_number);

	free(ctx.parser_state->commands);
	free(ctx.parser_state->arguments);
	free(ctx.parser_state->redirections);

	argument->command = ctx.interpreter_state;
	free(code);
}


static void
validate_identifier_name(struct argument *argument, const char *type, const char *reserved_word)
{
	const char *s;

	if (!argument->text[0] || isdigit(argument->text[0]))
		goto illegal;

	for (s = argument->text; *s; s++)
		if (!isalpha(*s) && !isdigit(*s) && *s != '_')
			goto illegal;

	return;

illegal:
	eprintf("illegal %s \"%s\" at line %zu for '%s'\n",
		type, argument->text, argument->line_number, reserved_word);
}


static void
interpret_unquoted_text(struct argument **argumentp)
{
	struct argument *argument = *argumentp;
	struct argument *new_argument;
	char *text = argument->text;
	char *beginning = text, *end = text;
	size_t addendum_length;
	int can_append = 1;

	while (*end && *end != '$')
		end++;

	if (!*end)
		return;

	if (end != beginning) {
		argument->length = (size_t)(end - beginning);
		argument->text = emalloc(argument->length + 1);
		memcpy(argument->text, beginning, argument->length);
		argument->text[argument->length] = '\0';
	}

	do {
		beginning = &end[1];
		switch (*beginning) {
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			if (isdigit(beginning[1])) {
				weprintf("multiple digits found immediately after '$' at line %zu, "
				         "only taking one for position argument\n", argument->line_number);
			}
			/* fall through */
		case '@':
		case '*':
		case '?':
		case '#':
		case '-':
		case '$':
		case '!':
			end = &beginning[1];
			break;
		case '~':
			if (check_extension("$~", argument->line_number)) {
				/* Get user home, so you can use it in arguments (in the way Bash allows ~ to be used;
				 * be we cannot because we don't want to violate POSIX needlessly) that look like
				 * variable assignments. Instead of limiting usernames to [a-z_][a-z0-9_-]*[$]?
				 * we will limit them only to [a-zA-Z0-9_-]\+[$]? and accept $ at the end even though
				 * it is stupid */
				end = &beginning[1];
				if (isalpha(*end) || isdigit(*end) || *end == '0' || *end == '-') {
					for (end = &end[1]; *end; end++)
						if (!isalpha(*end) && !isdigit(*end) && *end != '0' && *end != '-')
							break;
					if (*end == '$')
						end = &end[1];
				}
			} else {
				beginning--;
				goto append_text;
			}
			break;
		default:
			if (isalpha(*beginning) || *beginning == '_') {
				for (end = &beginning[1]; isdigit(*end) || isalpha(*end) || *end == '_'; end++);
			} else {
				beginning--;
				goto append_text;
			}
		}

		new_argument = ecalloc(1, sizeof(*new_argument));
		new_argument->next_part = argument->next_part;
		argument = *argumentp = argument->next_part = new_argument;
		argument->type = VARIABLE;
		argument->length = (size_t)(end - beginning);
		argument->text = emalloc(argument->length + 1);
		memcpy(argument->text, beginning, argument->length);
		argument->text[argument->length] = '\0';

		beginning = end;
		can_append = 0;

	append_text:
		while (*end && *end != '$')
			end++;

		if (end != beginning) {
			if (can_append) {
				addendum_length = (size_t)(end - beginning);
				argument->text = erealloc(argument->text, argument->length + addendum_length + 1);
				memcpy(&argument->text[argument->length], beginning, addendum_length);
				argument->length += addendum_length;
				argument->text[argument->length] = '\0';
			} else {
				new_argument = ecalloc(1, sizeof(*new_argument));
				new_argument->next_part = argument->next_part;
				argument = *argumentp = argument->next_part = new_argument;
				argument->type = UNQUOTED;
				argument->length = (size_t)(end - beginning);
				argument->text = emalloc(argument->length + 1);
				memcpy(argument->text, beginning, argument->length);
				argument->text[argument->length] = '\0';
			}
			can_append = 1;
		}

	} while (*end);

	free(text);
}


static void
translate_text_argument(struct argument *argument)
{
	struct interpreter_state *nested_state;

	for (; argument; argument = argument->next_part) {
		switch (argument->type) {
		case QUOTED:
			/* keep as is */
			break;

		case UNQUOTED:
			interpret_unquoted_text(&argument);
			break;

		case QUOTE_EXPRESSION:
		case ARITHMETIC_EXPRESSION:
		case ARITHMETIC_SUBSHELL:
			/* ARITHMETIC_EXPRESSION and ARITHMETIC_SUBSHELL can only be interpreted
			 * when evaluated as substitution can be used to insert operators */
			interpret_nested_code(argument, TEXT_ROOT, 0);
			break;

		case VARIABLE_SUBSTITUTION:
			interpret_nested_code(argument, VARIABLE_SUBSTITUTION_BRACKET, NEED_PREFIX_OR_VARIABLE_NAME);
			nested_state = argument->command;
			if (nested_state->requirement != NEED_INDEX_OR_OPERATOR_OR_END &&
			    nested_state->requirement != NEED_INDEX_OR_END &&
			    nested_state->requirement != NEED_OPERATOR_OR_END &&
			    nested_state->requirement != NEED_END) {
				eprintf("invalid variable substitution at line %zu\n", argument->line_number);
			}
			break;

		case BACKQUOTE_EXPRESSION:
		case SUBSHELL_SUBSTITUTION:
		case PROCESS_SUBSTITUTION_INPUT:
		case PROCESS_SUBSTITUTION_OUTPUT:
		case PROCESS_SUBSTITUTION_INPUT_OUTPUT:
		case SUBSHELL:
			interpret_nested_code(argument, CODE_ROOT, NEED_COMMAND);
			break;

		default:
		case COMMAND:
		case REDIRECTION:
		case FUNCTION_MARK:
		case VARIABLE:
			abort();
		}
	}
}


static void
push_redirection(struct command *command, struct argument **argumentp)
{
	struct redirection *redirection;
	struct argument *argument, *argument_end, *last_part;

	redirection = command->redirections[command->redirections_offset];
	command->redirections[command->redirections_offset] = NULL;
	command->redirections_offset += 1;

	argument = *argumentp;
	*argumentp = argument->next_part;

	redirection->right_hand_side = *argumentp;
	last_part = NULL;
	for (argument_end = redirection->right_hand_side; argument_end; argument_end = argument_end->next_part) {
		if (argument_end->type != QUOTED &&
		    argument_end->type != UNQUOTED &&
		    argument_end->type != QUOTE_EXPRESSION &&
		    argument_end->type != BACKQUOTE_EXPRESSION &&
		    argument_end->type != ARITHMETIC_EXPRESSION &&
		    argument_end->type != VARIABLE_SUBSTITUTION &&
		    argument_end->type != SUBSHELL_SUBSTITUTION)
			break;
		last_part = argument_end;
	}

	if (!last_part) {
		eprintf("missing right-hand side of '%s' at line %zu\n",
		        get_redirection_token(redirection->type), argument->line_number);
	}

	*argumentp = last_part->next_part;
	last_part->next_part = NULL;
	free(argument);

	if (redirection->left_hand_side)
		translate_text_argument(redirection->left_hand_side);
	translate_text_argument(redirection->right_hand_side);
}


static void
push_argument(struct parser_context *ctx, struct argument **argumentp)
{
	struct argument *argument = *argumentp, *last_part;

	if (argument->type == REDIRECTION || argument->type == FUNCTION_MARK) {
		*argumentp = argument->next_part;
		argument->next_part = NULL;

	} else {
		for (last_part = argument; last_part->next_part; last_part = last_part->next_part)
			if (last_part->next_part->type == REDIRECTION || last_part->next_part->type == FUNCTION_MARK)
				break;
		*argumentp = last_part->next_part;
		last_part->next_part = NULL;

		translate_text_argument(argument);
	}

	push_interpreted_argument(ctx, argument);
}


static void
push_typed_text(struct parser_context *ctx, struct argument *argument, char *text, size_t text_length, enum argument_type type)
{
	struct argument *new_argument;

	new_argument = ecalloc(1, sizeof(new_argument));
	new_argument->type = type;
	new_argument->line_number = argument->line_number;
	new_argument->length = text_length;	
	new_argument->text = emalloc(text_length + 1);
	memcpy(new_argument->text, text, text_length);
	new_argument->text[text_length] = '\0';

	push_interpreted_argument(ctx, new_argument);
}


static void
push_unquoted_segment(struct parser_context *ctx, struct argument *argument, char *text, size_t text_length) /* TODO (must handle $) */
{
}


static void
push_variable(struct parser_context *ctx, struct argument *argument, char *text, size_t text_length)
{
	push_typed_text(ctx, argument, text, text_length, VARIABLE);
}


static void
push_operator(struct parser_context *ctx, struct argument *argument, char *token, size_t token_length)
{
	push_typed_text(ctx, argument, token, token_length, OPERATOR);
}


static void
push_variable_substitution_argument(struct parser_context *ctx, struct command *command, struct argument **argumentp)
{
#define IS_SPECIAL_PARAMETER(C)\
	((C) == '@' || (C) == '*' || (C) == '?' || (C) == '#' || (C) == '$' || (C) == '!')

	struct argument *argument;
	size_t length, line_number;
	char *s;

	argument = *argumentp;
	*argumentp = argument->next_part;
	argument->next_part = NULL;

	line_number = argument->line_number;

	if (argument->type == UNQUOTED) {
		for (s = argument->text; *s;) {
			if (ctx->interpreter_state->requirement == NEED_PREFIX_OR_VARIABLE_NAME) {
				if (s[0] == '_' || isalnum(s[0]) || (s[0] == '~' && check_extension("~", line_number))) {
					ctx->interpreter_state->requirement = NEED_INDEX_OR_OPERATOR_OR_END;
				variable_or_tilde:
					length = 1;
					while (s[length] == '_' || isalnum(s[length]) || (s[0] == '~' && s[length] == '-'))
						length += 1;
					if (s[0] == '~' && s[length] == '$')
						length += 1;
					push_variable(ctx, argument, s, length);
					s = &s[length];
				} else if (IS_SPECIAL_PARAMETER(s[1])) {
					if (s[0] == '!' && check_extension("!", line_number))
						ctx->interpreter_state->requirement = NEED_INDEX_OR_SUFFIX_OR_END;
					else if (s[0] == '#')
						ctx->interpreter_state->requirement = NEED_INDEX_OR_END;
					else
						goto bad_syntax;
					push_operator(ctx, argument, &s[0], 1);
					push_variable(ctx, argument, &s[1], 1);
					s = &s[2];
				} else if (s[1] == '_' || isalnum(s[1]) || (s[1] == '~' && check_extension("~", line_number))) {
					if (s[0] == '!' && check_extension("!", line_number))
						ctx->interpreter_state->requirement = NEED_INDEX_OR_SUFFIX_OR_END;
					else if (s[0] == '#')
						ctx->interpreter_state->requirement = NEED_INDEX_OR_END;
					else
						goto bad_syntax;
					push_operator(ctx, argument, s, 1);
					s = &s[1];
					goto variable_or_tilde;
				} else if (IS_SPECIAL_PARAMETER(s[0])) {
					ctx->interpreter_state->requirement = NEED_INDEX_OR_OPERATOR_OR_END;
					push_variable(ctx, argument, s, 1);
					s = &s[1];
				} else {
					goto bad_syntax;
				}

			} else if (ctx->interpreter_state->requirement == NEED_INDEX_OR_OPERATOR_OR_END) {
				if (s[0] == '[' && check_extension("[", line_number)) {
					ctx->interpreter_state->requirement = NEED_OPERATOR_OR_END;
				index:
					/* TODO push INDEX substate that exits on ] */
				} else {
				operator:
					ctx->interpreter_state->requirement = NO_REQUIREMENT;
					if (s[0] == ':' && (s[1] == '-' || s[1] == '=' || s[1] == '?' || s[1] == '+')) {
						length = 2;
					} else if (s[0] == '-' || s[0] == '=' || s[0] == '?' || s[0] == '+') {
						length = 1;
					} else if (s[0] == '%' || s[0] == '#' ||
					          (s[0] == ',' && check_extension(s[1] == s[0] ? ",," : ",", line_number)) ||
					          (s[0] == '^' && check_extension(s[1] == s[0] ? "^^" : "^", line_number))) {
						if (s[1] == s[0])
							length = 2;
						else
							length = 1;
					} else if (s[0] == '/' && check_extension("/", line_number)) {
						ctx->interpreter_state->requirement = NEED_TEXT_OR_SLASH;
						length = 1;
					} else if (s[0] == ':' && check_extension(":", line_number)) {
						ctx->interpreter_state->requirement = NEED_TEXT_OR_COLON;
						length = 1;
					} else if (s[0] == '@' && check_extension("@", line_number)) {
						ctx->interpreter_state->requirement = NEED_AT_OPERAND;
						length = 1;
					} else {
						goto bad_syntax;
					}
					push_operator(ctx, argument, s, 2);
					s = &s[length];
				}

			} else if (ctx->interpreter_state->requirement == NEED_INDEX_OR_SUFFIX_OR_END) {
				ctx->interpreter_state->requirement = NEED_END;
				if (s[0] == '[') { /* Do not check if extensions are allowed, cannot reach this code otherwise */
					goto index;
				} else if (s[0] == '*' || s[0] == '@') {
					push_operator(ctx, argument, s, 1);
					s = &s[1];
				} else {
					goto bad_syntax;
				}

			} else if (ctx->interpreter_state->requirement == NEED_INDEX_OR_END) {
				ctx->interpreter_state->requirement = NEED_END;
				if (s[0] == '[') {
					goto index;
				} else {
					goto bad_syntax;
				}

			} else if (ctx->interpreter_state->requirement == NEED_OPERATOR_OR_END) {
				if (s[0] == '[')
					goto bad_syntax;
				else
					goto operator;

			} else if (ctx->interpreter_state->requirement == NEED_END) {
				goto bad_syntax;

			} else if (ctx->interpreter_state->requirement == NEED_AT_OPERAND) {
				if (*s == 'U' || *s == 'u' || *s == 'L' || *s == 'Q' || *s == 'E' ||
				    *s == 'P' || *s == 'A' || *s == 'K' || *s == 'a') {
					ctx->interpreter_state->requirement = NEED_END;
					push_operator(ctx, argument, s, 1);
					s = &s[1];
				} else {
					goto bad_syntax;
				}

			} else if (ctx->interpreter_state->requirement == NEED_TEXT_OR_SLASH) {
				length = 0;
				while (s[length] && s[length] != '/')
					length += 1;
				if (length) {
					push_unquoted_segment(ctx, argument, s, length);
					s = &s[length];
				}
				if (s[0]) {
					ctx->interpreter_state->requirement = NO_REQUIREMENT;
					push_operator(ctx, argument, s, 1);
					s = &s[1];
				}

			} else if (ctx->interpreter_state->requirement == NEED_TEXT_OR_COLON) {
				length = 0;
				while (s[length] && s[length] != ':')
					length += 1;
				if (length) {
					push_unquoted_segment(ctx, argument, s, length);
					s = &s[length];
				}
				if (s[0]) {
					ctx->interpreter_state->requirement = NO_REQUIREMENT;
					push_operator(ctx, argument, s, 1);
					s = &s[1];
				}

			} else {
				push_unquoted_segment(ctx, argument, s, length);
			}
		}
		free(argument->text);
		free(argument);
	} else {
		if (ctx->interpreter_state->requirement != NO_REQUIREMENT &&
		    ctx->interpreter_state->requirement != NEED_TEXT_OR_SLASH &&
		    ctx->interpreter_state->requirement != NEED_TEXT_OR_COLON) {
			goto bad_syntax;
		} else if (argument->type == QUOTED) {
			push_interpreted_argument(ctx, argument);
		} else {
			push_argument(ctx, &argument);
		}
	}

	return;

bad_syntax:
	eprintf("stray '%c' in bracketed variable substitution at line %zu\n", *s, line_number);

#undef IS_SPECIAL_PARAMETER
}


void
interpret_and_eliminate(struct parser_context *ctx)
{
	size_t interpreted = 0, arg_i;
	struct command *command;
	struct argument *argument, *next_argument;
	enum reserved_word reserved_word;

	if (ctx->here_document_stack && ctx->here_document_stack->first) {
		ctx->here_document_stack->interpret_when_empty = 1;
		return;
	}

	for (; ctx->interpreter_offset < ctx->parser_state->ncommands; ctx->interpreter_offset++) {
		command = ctx->parser_state->commands[ctx->interpreter_offset];
		argument = NULL;

		if (ctx->interpreter_state->dealing_with == TEXT_ROOT) {
			ctx->interpreter_state->requirement = NEED_VALUE;
		} else if (ctx->interpreter_state->dealing_with != FOR_STATEMENT &&
		           ctx->interpreter_state->dealing_with != VARIABLE_SUBSTITUTION_BRACKET) {
			ctx->interpreter_state->requirement = NEED_COMMAND;
		}

		for (arg_i = 0; argument || arg_i < command->narguments; arg_i += !argument) {
			if (!argument)
				argument = command->arguments[arg_i];

			/* TODO Implement alias substitution
			 * 
			 * Unless a word was quoted/backslashed, it is subject
			 * to alias substitution if it is the first argument
			 * of a command (after any previous alias substitution)
			 * or if it immediately follows an alias substitution
			 * resulting in an unquoted whitespace at the end.
			 * However, if the word is a reserved word (which may
			 * indeed the name of an alias) it shall not be subject
			 * to alias substitution if it has meaning in the context
			 * it appears in (for example: alias while=x be expanded
			 * for followed by the expansion of alias echo='echo '
			 * but not if it is the first word in a command). Creating
			 * aliases named after reserved words is stupid and we
			 * should only allow it in POSIX mode.
			 * 
			 * (Alias substitution occurs before the grammar is
			 * interpreted, meaning definition an alias does not
			 * modify already declared function that use a command
			 * with the same name as the alias.)
			 * 
			 * The result of alias substition is subject to
			 * alias substition, however (to avoid infinite loop),
			 * already expanded aliases shall not be recognised.
			 */

			if (ctx->interpreter_state->requirement == NEED_COMMAND &&
			    (reserved_word = get_reserved_word(argument))) {
				switch (reserved_word) {
				case BANG:
					if (ctx->interpreter_state->disallow_bang)
						stray_reserved_word(argument);
					ctx->interpreter_state->disallow_bang = 1;
					ctx->interpreter_state->have_bang = 1;
					break;

				case OPEN_CURLY:
				open_curly:
					push_state(ctx, CURLY_NESTING, argument->line_number);
					goto new_command;

				case CLOSE_CURLY:
					if (ctx->interpreter_state->dealing_with != CURLY_NESTING)
						stray_reserved_word(argument);
					pop_state(ctx);
					ctx->interpreter_state->requirement = NEED_COMMAND_END;
					break;

				case CASE: /* (TODO) */
					eprintf("reserved word 'case' (at line %zu) has not been implemented yet\n",
					        argument->line_number);
					/* NEWLINEs surrounding 'in' shall be ignored; ';' is not allowed */
					break;

				case DO:
					if (ctx->interpreter_state->dealing_with != REPEAT_CONDITIONAL)
						stray_reserved_word(argument);
					pop_state(ctx);
				do_keyword:
					push_state(ctx, DO_CLAUSE, argument->line_number);
					goto new_command;

				case DONE:
					if (ctx->interpreter_state->dealing_with != DO_CLAUSE)
						stray_reserved_word(argument);
					pop_state(ctx);
					pop_state(ctx);
					ctx->interpreter_state->requirement = NEED_COMMAND_END;
					break;

				case ELIF:
					if (ctx->interpreter_state->dealing_with != IF_CLAUSE)
						stray_reserved_word(argument);
					pop_state(ctx);
					push_state(ctx, IF_CONDITIONAL, argument->line_number);
					goto new_command;

				case ELSE:
					if (ctx->interpreter_state->dealing_with != IF_CLAUSE)
						stray_reserved_word(argument);
					pop_state(ctx);
					push_state(ctx, ELSE_CLAUSE, argument->line_number);
					goto new_command;

				case ESAC:
					stray_reserved_word(argument);
					break;

				case FI:
					if (ctx->interpreter_state->dealing_with != IF_CLAUSE &&
					    ctx->interpreter_state->dealing_with != ELSE_CLAUSE)
						stray_reserved_word(argument);
					pop_state(ctx);
					pop_state(ctx);
					ctx->interpreter_state->requirement = NEED_COMMAND_END;
					break;

				case FOR:
					push_state(ctx, FOR_STATEMENT, argument->line_number);
					ctx->interpreter_state->requirement = NEED_VARIABLE_NAME;
					free_text_argument(&argument);
					ctx->interpreter_state->allow_newline = 1;
					continue;

				case IF:
					push_state(ctx, IF_STATEMENT, argument->line_number);
					push_state(ctx, IF_CONDITIONAL, argument->line_number);
					goto new_command;

				case IN:
					stray_reserved_word(argument);
					break;

				case THEN:
					if (ctx->interpreter_state->dealing_with != IF_CONDITIONAL)
						stray_reserved_word(argument);
					pop_state(ctx);
					push_state(ctx, IF_CLAUSE, argument->line_number);
					goto new_command;

				case UNTIL:
					push_state(ctx, UNTIL_STATEMENT, argument->line_number);
					push_state(ctx, REPEAT_CONDITIONAL, argument->line_number);
					goto new_command;

				case WHILE:
					push_state(ctx, WHILE_STATEMENT, argument->line_number);
					push_state(ctx, REPEAT_CONDITIONAL, argument->line_number);
					goto new_command;

				default:
				case NOT_A_RESERVED_WORD:
					abort();
				}

				free_text_argument(&argument);
				ctx->interpreter_state->allow_newline = 0;
				continue;

			new_command:
				ctx->interpreter_state->requirement = NEED_COMMAND;
				free_text_argument(&argument);
				ctx->interpreter_state->allow_newline = 1;
				continue;

			} else if (ctx->interpreter_state->dealing_with == VARIABLE_SUBSTITUTION_BRACKET) {
				push_variable_substitution_argument(ctx, command, &argument);

			} else if (argument->type == REDIRECTION) {
				if (ctx->interpreter_state->dealing_with == FOR_STATEMENT)
					stray_redirection(command, argument);
				push_redirection(command, &argument);
				if (ctx->interpreter_state->requirement != NEED_FUNCTION_BODY)
					ctx->interpreter_state->requirement = NO_REQUIREMENT; /* e.g. "<somefile;" is ok */

			} else if (argument->type == FUNCTION_MARK) {
				if (ctx->interpreter_state->requirement == NEED_FUNCTION_BODY ||
				    ctx->interpreter_state->requirement == NEED_COMMAND_END ||
				    ctx->interpreter_state->narguments != 1 ||
				    ctx->interpreter_state->dealing_with == FOR_STATEMENT)
					eprintf("stray '()' at line %zu\n", argument->line_number);

				next_argument = argument->next_part;
				argument->next_part = NULL;
				push_argument(ctx, &argument);

				/* swap position of () and function name to make it easier to identify */
				argument = ctx->interpreter_state->arguments[0];
				ctx->interpreter_state->arguments[0] = ctx->interpreter_state->arguments[1];
				ctx->interpreter_state->arguments[1] = argument;

				argument = next_argument;
				ctx->interpreter_state->requirement = NEED_FUNCTION_BODY;
				ctx->interpreter_state->allow_newline = 1;

			} else if (ctx->interpreter_state->requirement == NEED_FUNCTION_BODY) {
				reserved_word = get_reserved_word(argument);
				if (reserved_word == OPEN_CURLY) {
					goto open_curly;
				} else if (argument->type == SUBSHELL) {
					ctx->interpreter_state->requirement = NEED_COMMAND_END;
					push_argument(ctx, &argument);
				} else {
					eprintf("required function body or redirection at line %zu;\n", argument->line_number);
				}
				ctx->interpreter_state->allow_newline = 0;

			} else if (ctx->interpreter_state->requirement == NEED_VARIABLE_NAME) {
				if (ctx->interpreter_state->dealing_with == FOR_STATEMENT) {
					if (argument->type != UNQUOTED)
						eprintf("required variable name after 'for' at line %zu\n", argument->line_number);
					validate_identifier_name(argument, "variable name", "for");
					argument->type = VARIABLE;
					push_interpreted_argument(ctx, argument);
					ctx->interpreter_state->requirement = NEED_IN_OR_DO;
					ctx->interpreter_state->allow_newline = 1;
				} else {
					abort();
				}

			} else if (ctx->interpreter_state->requirement == NEED_DO) {
				reserved_word = get_reserved_word(argument);
				if (reserved_word != DO)
					stray_reserved_word(argument);
				goto do_keyword;

			} else if (ctx->interpreter_state->requirement == NEED_IN_OR_DO) {
				reserved_word = get_reserved_word(argument);
				if (reserved_word == DO) {
					push_command(ctx, command);
					goto do_keyword;
				} else if (reserved_word == IN) {
					ctx->interpreter_state->requirement = NEED_VALUE;
					ctx->interpreter_state->allow_newline = 0;
				} else {
					stray_reserved_word(argument);
				}

			} else {
				if (ctx->interpreter_state->requirement == NEED_COMMAND_END) {
					eprintf("required %s at line %zu after control statement\n",
					        "';', '&', '||', '&&', '|', '&|', '|&', '<>|', or redirection",
					        argument->line_number);
				}

				if (ctx->interpreter_state->requirement != NEED_VALUE)
					ctx->interpreter_state->requirement = NO_REQUIREMENT;
				if (argument->type == SUBSHELL || argument->type == ARITHMETIC_SUBSHELL)
					if (ctx->interpreter_state->narguments == 0)
						ctx->interpreter_state->requirement = NEED_COMMAND_END;

				push_argument(ctx, &argument);
				ctx->interpreter_state->allow_newline = 0;
			}
		}

		if (ctx->interpreter_state->dealing_with == TEXT_ROOT ||
		    ctx->interpreter_state->dealing_with == VARIABLE_SUBSTITUTION_BRACKET) {
			free(command->redirections);
			free(command->arguments);
			free(command);
			continue;
		}

		if (ctx->interpreter_state->allow_newline) {
			ctx->interpreter_state->allow_newline = 0;
			if (command->terminal == NEWLINE) {
				free(command->redirections);
				free(command->arguments);
				free(command);
				continue;
			}
		}

		if ((ctx->interpreter_state->requirement == NEED_COMMAND && command->narguments == arg_i) ||
		    ctx->interpreter_state->requirement == NEED_FUNCTION_BODY ||
		    ctx->interpreter_state->requirement == NEED_VARIABLE_NAME)
			stray_command_terminal(command);

		if (ctx->interpreter_state->requirement == NEED_IN_OR_DO) {
			ctx->interpreter_state->requirement = NEED_DO;
			if (command->terminal != SEMICOLON && command->terminal != NEWLINE)
				stray_command_terminal(command);
		}

		push_command(ctx, command);

		if (command->terminal == SEMICOLON ||
		    command->terminal == NEWLINE ||
		    command->terminal == AMPERSAND) {
			ctx->interpreter_state->disallow_bang = 0;
			if (ctx->interpreter_state->dealing_with == MAIN_BODY) {
				/* TODO execute and destroy queued up commands (also destroy list) */
				interpreted = ctx->interpreter_offset + 1;
			}
		} else if (command->terminal == DOUBLE_SEMICOLON) {
			stray_command_terminal(command);
		} else {
			ctx->interpreter_state->disallow_bang = 1;
		}
	}

	memmove(&ctx->parser_state->commands[0],
	        &ctx->parser_state->commands[interpreted],
	        ctx->parser_state->ncommands - interpreted);
	ctx->parser_state->ncommands -= interpreted;
	ctx->interpreter_offset -= interpreted;

	if (!ctx->parser_state->ncommands) {
		free(ctx->parser_state->commands);
		ctx->parser_state->commands = NULL;
	}
}
