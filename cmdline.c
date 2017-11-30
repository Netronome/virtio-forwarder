/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2016-2017 Netronome.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Netronome nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cmdline.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_USAGE_LEN 1024
#define _append(x) len += snprintf(s+len, MAX_USAGE_LEN-len, "%s", x)

int cmdline_optind;

void
cmdline_show_usage(const cmdline_opt_t *opts, int argc __attribute__((unused)),
			char **argv)
{
	const char *cmdname;
	const cmdline_opt_t *optstart = opts;
	unsigned int num = 0;
	char *s;
	unsigned int len = 0;

	s = malloc(MAX_USAGE_LEN + 1);
	s[0] = 0;

	cmdname = strrchr(argv[0], '/');
	if (cmdname)
		++cmdname;
	else
		cmdname = argv[0];

	while (opts->opt_long) {
		_append(" ");
		if ((opts->param_flags&CMDLINE_PARAM_FLAG_MANDATORY)==0)
			_append("[");
		_append("--");
		_append(opts->opt_long);
		if (opts->has_arg == 2)
			_append("[");
		if (opts->has_arg)
			_append("=<val>");
		if (opts->has_arg == 2)
			_append("]");
		if ((opts->param_flags & CMDLINE_PARAM_FLAG_MANDATORY) == 0)
			_append("]");
		++num;
		++opts;
	}

	opts = optstart;
	if (opts[num].usage == 0)
		fprintf(stderr, "USAGE: %s%s\n", cmdname, s);
	else
		fprintf(stderr, "USAGE: %s%s %s\n", cmdname, s,
			opts[num].usage);
	fprintf(stderr, "Options:\n");
	while (opts->opt_long) {
		char nameval[43];
		char *usage;
		char *pos1, *save1 = 0;
		snprintf(nameval, 43, "-%c%s or --%s%s", opts->opt_short,
			opts->has_arg ?
			(opts->has_arg > 1 ? "[<val>]" : "<val>  ")
			: "       ",
			opts->opt_long,
			opts->has_arg ?
			(opts->has_arg > 1 ? "[=<val>]" : "=<val>  ")
			: "       ");
		fprintf(stderr, "  %-43s ", nameval);
		usage = strdup(opts->usage);
		pos1 = strtok_r(usage, "\n", &save1);
		while (pos1) {
			char *pos2, *save2=0;
			unsigned int linelen;
			pos2 = strtok_r(pos1, " ", &save2);
			linelen = 0;
			while (pos2) {
				unsigned len = strlen(pos2);
				if ((linelen + len + 1) >= 80) {
					fprintf(stderr, "\n%-46s", " ");
					linelen=0;
				}
				fprintf(stderr, "%s ", pos2);
				linelen += (len+1);
				pos2 = strtok_r(0, " ", &save2);
			}
			pos1 = strtok_r(0, "\n", &save1);
			if (pos1)
				fprintf(stderr, "\n%-46s", " ");
		}
		free(usage);
		fprintf(stderr, "\n");
		++opts;
	}
	free(s);
}

#define MAX_OPTIONS 64

int
cmdline_parser(const cmdline_opt_t *opts, void *opaque, int argc, char **argv,
		unsigned int flags)
{
	struct option *getopt_options = 0;
	char *optstring = 0;
	unsigned int optlen;
	unsigned int num = 0;
	int ret = -1;
	unsigned int i;
	int c;
	uint64_t mandatory_bitmap = 0; /* max 64 options */
	uint64_t mandatory_check = 0;
	int terminate_check = 0;
	const cmdline_opt_t *optstart = opts;

	while (opts->opt_long) {
		++opts;
		++num;
	}
	if (num > MAX_OPTIONS) {
		if ((flags & CMDLINE_NO_STDERR) == 0)
			fprintf(stderr, "Max %u options supported!",
				MAX_OPTIONS);
		return -1;
	}
	getopt_options = malloc(sizeof(*getopt_options)*(num + 1));
	memset(getopt_options, 0, sizeof(*getopt_options)*(num + 1));
	optstring = malloc(num * 3 + 2); /* each option could be up to 3 characters, plus terminating null and starting colon */
	optlen = 0;
	optstring[optlen++] = ':'; /* to enable getopt indication of missing arguments */
	i = 0;
	opts = optstart;
	optind = 0;
	while (opts->opt_long) {
		optstring[optlen++] = opts->opt_short;
		getopt_options[i].val = opts->opt_short;
		getopt_options[i].name = opts->opt_long;
		if (opts->has_arg) {
			optstring[optlen++] = ':';
			getopt_options[i].has_arg = opts->has_arg;
			if (opts->has_arg > 1)
				optstring[optlen++] = ':';
		}
		if ((opts->param_flags & CMDLINE_PARAM_FLAG_MANDATORY))
			mandatory_bitmap |= (1ULL<<i);
		++opts;
		++i;
	}
	optstring[optlen] = 0;
	opterr = 0; // disable getopt internal error printing
	while ((c = getopt_long(argc, argv, optstring, getopt_options, 0)) != -1) {
		for (i = 0; i < num; ++i) {
			if (optstart[i].opt_short == c)
				break;
		}
		if (i >= num) {
			if (c == '?') {
				// unexpected option
				if (optopt == 0) {
					continue;
				}
				if ((optopt!='?' && optopt!='h') &&
						(flags & CMDLINE_IGNORE_UNKNOWN))
					continue;
				if ((flags & CMDLINE_NO_STDERR) == 0) {
					if (optopt != '?' && optopt != 'h')
						fprintf(stderr, "Unknown option '%c' (%02x)\n",
							optopt, optopt);
					cmdline_show_usage(optstart, argc, argv);
				}
				ret = -1;
				goto exit;
			} else
			if (c == ':') {
				// missing required argument
				if ((flags & CMDLINE_NO_STDERR) == 0) {
					fprintf(stderr, "Missing argument for option '%c'\n", optopt);
					cmdline_show_usage(optstart, argc, argv);
				}
				ret = -1;
				goto exit;
			}
			ret = -1;
			goto exit;
		}
		if ((ret = optstart[i].func(opaque, optarg, i)) != 0) {
			if ((flags & CMDLINE_NO_STDERR) == 0) {
				fprintf(stderr, "Error with option '%c', usage is: %s\n",
					c, optstart[i].usage);
				cmdline_show_usage(optstart, argc, argv);
			}
			ret = -1;
			goto exit;
		}
		if (optstart[i].param_flags&CMDLINE_PARAM_FLAG_MANDATORY)
			mandatory_check|=(1ULL<<i);
		if (optstart[i].param_flags&CMDLINE_PARAM_FLAG_TERMINATE)
			terminate_check = 1;
	}
	if ((mandatory_check != mandatory_bitmap) && (terminate_check == 0)) {
		if ((flags & CMDLINE_NO_STDERR) == 0) {
			fprintf(stderr, "The following mandatory options were omitted:\n");
			for (i=0; i<num; ++i) {
				if ((mandatory_bitmap & (1ULL<<i)) &&
						(mandatory_check & (1ULL<<i)) == 0) {
					fprintf(stderr, "  -%c --%s %s\t%s\n",
						optstart[i].opt_short,
						optstart[i].opt_long,
						optstart[i].has_arg ?
						(optstart[i].has_arg > 1 ? "[<val>]" : "<val>  ")
						: "       ", optstart[i].usage);
				}
			}
			fprintf(stderr, "\n");
			cmdline_show_usage(optstart, argc, argv);
		}
		ret = -1;
		goto exit;
	}
	cmdline_optind = optind;
	if (optind >= 1 && optind < argc &&
			(flags & CMDLINE_IGNORE_EXTRA) == 0) {
		if (strcmp(argv[optind-1],"--") != 0) {
			if ((flags & CMDLINE_NO_STDERR) == 0) {
				fprintf(stderr, "Unhandled extra option '%s'\n",
					argv[optind]);
				cmdline_show_usage(optstart, argc, argv);
			}
			ret = -1;
			goto exit;
		}
	}
	ret = 0;

exit:
	if (getopt_options) {
		free(getopt_options);
		getopt_options = 0;
	}
	if (optstring) {
		free(optstring);
		optstring = 0;
	}

	return ret;
}

/*
 * Unit test, can be compiled as follows:
 * gcc cmdline.c -DCMDLINE_UNITTEST -o cmdline
 */
#ifdef CMDLINE_UNITTEST
#include <stdio.h>
#include <stdlib.h>
#include "cmdline.h"

#define _ASSERT(x) if (!!(x)==0) { fprintf(stderr, "Assertion '"#x"' on line %u failed!\n", __LINE__); abort(); }

struct testcmdline {
	int i;
	const char *s;
	int one, two;
	int noarg;
	const char *optarg;
	int a,b,c;
	int showversion;
};

int tst_set_i(void *opaque, const char *arg, int index)
{
	int i;
	struct testcmdline *t = (struct testcmdline *)opaque;

	i = atoi(arg);
	if (i<1 || i>255)
		return -1;
	t->i = i;

	return 0;
}

int tst_set_s(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	t->s = arg;

	return 0;
}

int tst_set_1(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	if (t->two != -1)
		return -1;
	t->one = atoi(arg);

	return 0;
}

int tst_set_2(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	if (t->one == -1)
		return -1;
	t->two = atoi(arg);

	return 0;
}

int tst_set_noarg(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	t->noarg = 1;

	return 0;
}

int tst_set_optarg(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	if (arg)
		t->optarg = arg;
	else
		t->optarg = 0;

	return 0;
}

int tst_show_version(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	t->showversion = 1;

	return 0;
}

int tst_set_index(void *opaque, const char *arg, int index);

cmdline_opt_t opts[] = {
	{"int", 'i', CMDLINE_PARAM_FLAG_MANDATORY, tst_set_i, 1, "An integer in the range 1-255"},
	{"string", 's', CMDLINE_PARAM_FLAG_MANDATORY, tst_set_s, 1, "A string"},
	{"one", '1', 0, tst_set_1, 1, "An integer, must be set before two"},
	{"two", '2', 0, tst_set_2, 1, "An integer, must be set after one"},
	{"noarg", 'n', 0, tst_set_noarg, 0, "Option without an argument"},
	{"optarg", 'o', 0, tst_set_optarg, 2, "Option with optional argument"},
	{"aopt", 'a', 0, tst_set_index, 1, "A option"},
	{"bopt", 'b', 0, tst_set_index, 1, "B option"},
	{"copt", 'c', 0, tst_set_index, 1, "C option"},
	{"version", 'v', CMDLINE_PARAM_FLAG_TERMINATE, tst_show_version, 0, "Show version and exit"},
	{ 0, 0, 0, 0, 0, "[filename]" }
};

/* Test callback using index to refer back to item in option array. */
int tst_set_index(void *opaque, const char *arg, int index)
{
	struct testcmdline *t = (struct testcmdline *)opaque;

	switch (opts[index].opt_short) {
	case 'a':
		t->a = atoi(arg);
		break;
	case 'b':
		t->b = atoi(arg);
		break;
	case 'c':
		t->c = atoi(arg);
		break;
	default:
		return -1;
		break;
	}

	return 0;
}

int main()
{
	{
		/* Test missing mandatory. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42", 0
		};
		int argc = 3;
		memset(&t, 0, sizeof(t));
		/* Must return error since mandatory argument -s is missing. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, CMDLINE_NO_STDERR) != 0);
	}
	{
		/* Test missing mandatory but with terminating option. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-v", 0
		};
		int argc = 4;
		memset(&t, 0, sizeof(t));
		/* Must return success despite mandatory argument -s being
		 * missing because terminating arg -v was given. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(t.i==42);
		_ASSERT(t.showversion==1);
	}
	{
		/* test mandatory. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo", 0
		};
		int argc = 5;
		memset(&t, 0, sizeof(t));
		/* Must return success since all mandatory arguments are present. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(t.i==42);
		_ASSERT(strcmp(t.s,"foo")==0);
	}
	{
		/* Test extra non-options. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"blah", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		/* Must return failure since there is an extra non-option argument. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, CMDLINE_NO_STDERR) != 0);
	}
	{
		/* test extra non-options. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"blah", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		/* Must return success now since we're ignoring non-options arguments. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, CMDLINE_IGNORE_EXTRA) == 0);
		/* Ensure cmdline_optind gives us the correct string here. */
		_ASSERT(strcmp(argv[cmdline_optind],"blah")==0);
	}
	{
		/*Test unknown option. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"-X", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		/* Must return failure since there is an unknown option argument. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, CMDLINE_NO_STDERR) != 0);
	}
	{
		/* Test unknown option. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"-X", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		/* Must return success since we're now ignoring the unknown option. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, CMDLINE_IGNORE_UNKNOWN) == 0);
		_ASSERT(cmdline_optind==argc);
	}
	{
		/* Test optional arg. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"-o", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		t.optarg = (char*)-1;
		/* Must return success. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(t.optarg==0);
	}
	{
		/* Test optional arg. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"-obar", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		t.optarg = (char*)-1;
		/* Must return success. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(strcmp(t.optarg,"bar")==0);
	}
	{
		/* Test long optional arg. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"--optarg=bar", 0
		};
		int argc = 6;
		memset(&t, 0, sizeof(t));
		t.optarg = (char*)-1;
		/* Must return success. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(strcmp(t.optarg,"bar")==0);
	}
	{
		/* Test index in callback. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foo",
			"-a","1",
			"-b","2",
			"-c","3", 0
		};
		int argc = 11;
		memset(&t, 0, sizeof(t));
		/* Must return success. */
		_ASSERT(cmdline_parser(opts, &t, argc, argv, 0) == 0);
		_ASSERT(t.a==1 && t.b==2 && t.c==3);
	}
	{
		/* Test various combinations together. */
		struct testcmdline t;
		char *argv[] = {
			"cmdline",
			"-i", "42",
			"-s", "foofoo",
			"--noarg",
			"-ofoobar",
			"-1", "1",
			"-2", "2",
			"blah", 0
		};
		int argc = 12;
		memset(&t, 0, sizeof(t));
		t.i = -1;
		t.s = 0;
		t.one = -1;
		t.two = -1;
		t.noarg = -1;
		_ASSERT(cmdline_parser(opts, &t, argc, argv,
					CMDLINE_IGNORE_EXTRA |
					CMDLINE_IGNORE_UNKNOWN) == 0);
		_ASSERT(t.one != -1 && t.two != -1);
		_ASSERT(cmdline_optind < argc);
		_ASSERT(strcmp(argv[cmdline_optind],"blah")==0);
		_ASSERT(t.noarg==1);
		_ASSERT(strcmp(t.optarg,"foobar")==0);
		_ASSERT(strcmp(t.s, "foofoo")==0);
		_ASSERT(t.i==42);
		_ASSERT(t.one==1 && t.two==2);
	}

	return (EXIT_SUCCESS);
}
#endif
