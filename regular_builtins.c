/* See LICENSE file for copyright and license details. */
#include "common.h"


int
true_main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	return 0;
}


int
false_main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	return 1;
}


BUILTIN_USAGE(pwd_usage, "[-L | -P]")
int
pwd_main(int argc, char **argv)
{
	void (*usage)(void) = pwd_usage;
        int physical = 0;
        char *cwd = NULL;
        size_t size = 64 / 2;
        const char *pwd;
	struct stat cst, pst;

	ARGBEGIN {
	case 'L':
		physical = 0;
		break;
	case 'P':
		physical = 1;
		break;
	default:
		usage();
	} ARGEND;

	if (argc)
		weprintf("ignoring operands"); /* other implementations either warn or are silent, they don't fail */

	for (;;) {
		cwd = erealloc(cwd, size *= 2);
		if (getcwd(cwd, size))
			break;
		if (errno != ERANGE)
			eprintf("getcwd %zu:", size);
	}

	if (physical || !(pwd = getenv("PWD")) || *pwd != '/' || stat(pwd, &pst) || stat(cwd, &cst))
		puts(cwd);
	else if (pst.st_dev == cst.st_dev && pst.st_ino == cst.st_ino)
		puts(pwd);
	else
		puts(cwd);

	free(cwd);
	if (fflush(stdout) || ferror(stdout))
		weprintf("fflush <stdout>:");
	return 0;
}
