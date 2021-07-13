/* See LICENSE file for copyright and license details. */
#include "common.h"

USAGE("");


int login_shell;
int posix_mode;


void
initialise_parser_context(struct parser_context *ctx, int need_tokeniser, int need_parser)
{
	memset(ctx, 0, sizeof(*ctx));
	if (need_tokeniser) {
		ctx->preparser_line_number = 1;
		ctx->tokeniser_line_number = 1;
		ctx->mode_stack = ecalloc(1, sizeof(*ctx->mode_stack));
		ctx->mode_stack->she_is_comment = 1;
		ctx->here_document_stack = ecalloc(1, sizeof(*ctx->here_document_stack));
		ctx->here_document_stack->next = &ctx->here_document_stack->first;
	}
	if (need_parser) {
		ctx->parser_state = ecalloc(1, sizeof(*ctx->parser_state));
	}
	ctx->interpreter_state = ecalloc(1, sizeof(*ctx->interpreter_state));
}


static int
is_sh(char *name)
{
	if (!strcmp(name, "sh"))
		return 1;
	name = strrchr(name, '/');
	return name && !strcmp(name, "/sh");
}


int
main(int argc, char *argv[])
{
	struct parser_context ctx;
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t buffer_head = 0;
	size_t buffer_tail = 0;
	ssize_t r;
	size_t n, nremoved;

	ARGBEGIN {
	default:
		usage();
	} ARGEND;

	if (argc)
		usage();

	login_shell = (argv0[0] == '-');
	posix_mode = is_sh(&argv0[login_shell]);

	initialise_parser_context(&ctx, 1, 1);
	ctx.tty_input = (char)isatty(STDIN_FILENO);
	if (ctx.tty_input)
		weprintf("apsh is currently not implemented to be interactive\n");

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
		buffer_tail += n = parse(&ctx, &buffer[buffer_tail], buffer_head - buffer_tail, &nremoved);
		buffer_head -= nremoved;
	}

	ctx.end_of_file_reached = 1;
	buffer_tail += parse(&ctx, &buffer[buffer_tail], buffer_head - buffer_tail, &nremoved);
	buffer_head -= nremoved;
	if (buffer_tail != buffer_head || ctx.premature_end_of_file)
		eprintf("premature end of file reached\n");

	free(ctx.parser_state->commands);
	free(ctx.parser_state->arguments);
	free(ctx.parser_state->redirections);
	free(ctx.parser_state);
	free(ctx.here_document_stack);
	free(ctx.interpreter_state);
	free(buffer);
	return 0;
}
