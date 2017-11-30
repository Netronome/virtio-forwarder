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

#ifndef CMDLINE_H
#define CMDLINE_H

#ifdef	__cplusplus
extern "C" {
#endif

extern int cmdline_optind;

/**
 * Option callback function definition.
 * The user should define a function with the following prototype for each cmdline
 * option supported by his application. The callback is called when the relevant
 * option is found in the processing of cmdline_parse().
 *
 * @param opaque The opaque value (provided by the caller when calling cmdline_parse()). This
 * could typically contain a pointer to a struct holding the user's config variables
 * to be populated.
 * @param arg The option argument, NULL if no argument
 * @param opt_index The index in the opts array (provided to cmdline_parse()) of
 * the matching option.
 * @return The function must return 0 for success, or non-zero otherwise (e.g. if the
 * argument has invalid syntax). Ideally the usage information for the cmdline
 * option should describe the valid syntax for the option's argument, since by
 * default cmdline_parse() will display the usage info if this function returns an error.
 */
typedef int (*cmdline_opt_fn)(void *opaque, const char *arg, int opt_index);

#define CMDLINE_PARAM_FLAG_MANDATORY 1 ///< This option must be supplied for app to be able to run
#define CMDLINE_PARAM_FLAG_TERMINATE 2 ///< This option causes app to terminate (e.g. output version info to stdout), presence of such a param ignores any missin CMDLINE_PARAM_FLAG_MANDATORY params

///< Struct for each option (an array of these is passed to cmdline_parse)
typedef struct {
	const char *opt_long; ///< Long form of argument name (mandatory)
	const char opt_short; ///< Short form of argument name (mandatory)
	unsigned param_flags; ///< bitmap of CMDLINE_PARAM_FLAG*
	cmdline_opt_fn func;  ///< Callback function for this argument (mandatory)
	int has_arg; ///< 0 for no argument, 1 for mandatory argument, 2 for optional argument
	const char *usage; ///< Usage info for this argument
} cmdline_opt_t;

#define CMDLINE_NO_STDERR 1
#define CMDLINE_IGNORE_UNKNOWN 2
#define CMDLINE_IGNORE_EXTRA 4

/**
 * Parse command line options, calling a user defined callback function for
 * each valid option. Any errors cause cmdline_show_usage() to be called and
 * non-zero value is returned. Command line processing stops if "--" is
 * encountered, and cmdline_optind will then be left with a value less
 * than argc (remaining argv items can thus be processed manually further by the user).
 *
 * Some optional flags can be passed to alter the default behaviour:
 * CMDLINE_NO_STDERR: disable all stderr output (including usage info)
 * CMDLINE_IGNORE_UNKNOWN: ignore unknown options (default otherwise is to return error)
 * CMDLINE_IGNORE_EXTRA: ignore extra non-option values (default otherwise is to return error)
 *
 * @param opts	  Array of cmdline_opt_t structs, last value MUST have opt_long==NULL.
 * @param opaque  Opaque value passed to argument callback functions, typically
 * pointer to a custom struct containing the config option variables.
 * @param argc	  Argument count from main()
 * @param argv	  Argument values from main()
 * @param flags   Optional bitwise-OR combination of CMDLINE_* flag values (0 for defaults)
 * @return 0 indicates success
 */
int
cmdline_parser(const cmdline_opt_t *opts, void *opaque, int argc, char **argv,
		unsigned int flags);

/**
 * Outputs usage information to stderr. The cmdline syntax is automatically
 * derived from the opts array, and a list containing detailed usage of each
 * option is displayed after the syntax line.
 *
 * @param opts	  Array of cmdline_opt_t structs, last value MUST have opt_long==NULL.
 *				  This last NULL value can optionally have some text in the usage field;
 *				  if this is not NULL, it is appended to the end of the cmdline
 *				  syntax prior to the detailed options' usage info (can be used to
 *				  display additional instructions).
 * @param argc	  Argument count from main()
 * @param argv	  Argument values from main()
 */
void cmdline_show_usage(const cmdline_opt_t *opts, int argc, char **argv);

#ifdef	__cplusplus
}
#endif

#endif	/* CMDLINE_H */
