/* See LICENSE file for copyright and license details. */
#include "common.h"


size_t
parse(struct parser_context *ctx, char *code, size_t code_len, size_t *nremovedp)
{
	int end_of_file_reached;
	size_t bytes_parsed = 0;

	end_of_file_reached = ctx->end_of_file_reached;
	ctx->end_of_file_reached = 0;
	*nremovedp = 0;

	while (ctx->preparser_offset < code_len) {
		if (code[ctx->preparser_offset] == '\0') {
			if (!ctx->tty_input)
				weprintf("ignoring NUL byte at line %zu\n", ctx->preparser_line_number);
			memmove(&code[ctx->preparser_offset],
			        &code[ctx->preparser_offset + 1],
			        (code_len -= 1) - ctx->preparser_offset);
			*nremovedp += 1;

		} else if (code[ctx->preparser_offset] == '\n') {
			ctx->preparser_line_number += 1;
			ctx->preparser_offset += 1;

		} else if (code[ctx->preparser_offset] == '\\') {
			if (ctx->preparser_offset + 1 == code_len)
				break;
			if (code[ctx->preparser_offset + 1] == '\n') {
				bytes_parsed += parse_preparsed(ctx, &code[bytes_parsed], ctx->preparser_offset - bytes_parsed);
				memmove(&code[ctx->preparser_offset],
				        &code[ctx->preparser_offset + 2],
				        (code_len -= 2) - ctx->preparser_offset);
				*nremovedp += 2;
				ctx->line_continuations += 1;
			} else {
				ctx->preparser_offset += 2;
			}

		} else {
			ctx->preparser_offset += 1;
		}
	}

	ctx->end_of_file_reached = end_of_file_reached;
	bytes_parsed += parse_preparsed(ctx, &code[bytes_parsed], ctx->preparser_offset - bytes_parsed);
	ctx->preparser_offset -= bytes_parsed;
	return bytes_parsed;
}
