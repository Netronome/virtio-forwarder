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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "argv.h"

int add_arg(char ***argvp, int *argcp, const char *arg)
{
	char **new_argv;

	if ((new_argv = realloc(*argvp, (*argcp + 2) * sizeof(char *))) == NULL)
		return -1;

	*argvp = new_argv;
	(*argvp)[(*argcp)++] = strdup(arg);
	(*argvp)[(*argcp)] = 0;

	return 0;
}

void free_args(char ***argvp, int *argcp)
{
	if (*argvp != NULL) {
		char **argv_ = *argvp;
		int argc_ = *argcp;
		while (argc_--)
			free(*argv_++);
		free(*argvp);
		*argvp = NULL;
		*argcp = 0;
	}
}

#ifdef _UNIT_TEST_ARGV
/* gcc -D_UNIT_TEST_ARGV argv.c -std=gnu11 -g -o argv */
#define _ASSERT(x) if (!!(x)==0) { fprintf(stderr, "Assertion '"#x"' on line %u failed!\n", __LINE__); abort(); }

void show_args(int argc, char **argv)
{
	if (argv != NULL) {
		while (argc--)
			printf("  %s\n", *argv++);
	}
}

int main(int argc, char **argv)
{
	char **n_argv = NULL;
	int n_argc = 0;

	add_arg(&n_argv, &n_argc, argv[0]);
	_ASSERT(n_argc == 1);
	_ASSERT(n_argv[n_argc - 1] != 0);
	_ASSERT(n_argv[n_argc] == 0);
	for (int i = 0; i < 10; ++i) {
		char buf[10];
		snprintf(buf, 10, "-%d", i);
		add_arg(&n_argv, &n_argc, buf);
		_ASSERT(n_argc == (i + 2));
		_ASSERT(n_argv[n_argc - 1] != 0);
		_ASSERT(n_argv[n_argc] == 0);
	}
	show_args(n_argc, n_argv);
	free_args(&n_argv, &n_argc);
	_ASSERT(n_argv == NULL);
	_ASSERT(n_argc == 0);

	return 0;
}
#endif /* _UNIT_TEST_ARGV */
