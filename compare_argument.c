#include "compare_argument.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

enum option_comparison_result compare_argument(const char option_name_char, const char *option_name, const char **argv, const char ***out_argv, unsigned *out_args_consumed, bool option_arg_optional, const char **out_option_arg) {
	if(!argv) {
		fprintf(stderr, "compare_argument: internal error: compare_argument received an argv of NULL (please tell the developer)\n");
		return option_comparison_error;
	}

	const char *arg = *argv;
	if(!arg) {
		fprintf(stderr, "compare_argument: internal error: compare_argument received an empty argv (please tell the developer)\n");
		return false;
	}

	enum option_comparison_result result = option_comparison_error;
	const char **orig_argv = argv;

	if(*(arg++) != '-') {
		//Not an option (options begin with -).
		result = option_comparison_notanopt;
	} else {
		if(*arg == '\0') {
			//Special filename for stdio (-).
			result = option_comparison_stdio;
		} else if(*arg == '-') {
			if(*++arg == '\0') {
				//Options terminator (--).
				result = option_comparison_endofflags;
			} else {
				if(!option_name) {
					//We're not expecting a long option, so fail the match immediately.
					result = option_comparison_nomatch;
				} else {
					//Long option (e.g. --file).
					size_t name_len = strlen(option_name);
					if(strncmp(arg, option_name, name_len) == 0) {
						//It's a match! Look for a value.
						if(!out_option_arg) {
							if(*arg == '=') {
								//The argument value follows.
								//Unfortunately, we weren't expecting an argument to this option. Return an error indicating this fact.
								result = option_comparison_optargunexpected;
							} else if(*arg != '\0') {
								//Not a match! We were fooled by restricting the search to name_len characters.
								//This argument's option name is longer than that, and the option name that we're looking for is a prefix of it.
								//But they are not equal, so this is not a match.
								result = option_comparison_nomatch;
							}
						} else {
							arg += name_len;
							if(*arg == '=') {
								//The argument value follows.
								++arg;
							} else if(*arg != '\0') {
								//Not a match! We were fooled by restricting the search to name_len characters.
								//This argument's option name is longer than that, and the option name that we're looking for is a prefix of it.
								//But they are not equal, so this is not a match.
								result = option_comparison_nomatch;
							} else {
								//If there's no arg after this, then that arg will be NULL (per C99), which is what we want to return through out_option_arg in that case.
								arg = *++argv;
								if(option_arg_optional && arg && (*arg == '-')) {
									//The option arg for our option is optional, and the arg that we found for it looks like another option. Return no option arg (NULL).
									arg = NULL;
									//If option_arg_optional was false, we would accept this arg even though it looks like an option (because we would not even check whether it looks like an option).
								}
							}
							*out_option_arg = arg;
						}
						result = option_comparison_longopt;
					} else {
						//No match.
						result = option_comparison_nomatch;
					}
				}
			}
		} else if(*arg == option_name_char) {
			//Short option (e.g. -f).
			if(out_option_arg) {
				if(*arg != '\0') ++arg;
				if(*arg == '\0') {
					//Expect two args; e.g. -f filename.
					//If there's no arg after this, then that arg will be NULL (per C99), which is what we want to return through out_option_arg in that case.
					arg = *++argv;
					if(option_arg_optional && arg && (*arg == '-')) {
						//The option arg for our option is optional, and the arg that we found for it looks like another option. Return no option arg (NULL).
						arg = NULL;
						//If option_arg_optional was false, we would accept this arg even though it looks like an option (because we would not even check whether it looks like an option).
					}
				}
				*out_option_arg = arg;
			}
			result = option_comparison_shortopt;
		}
	}

	if(out_args_consumed || out_argv)
		argv = (result > option_comparison_nomatch) ? ++argv : orig_argv;
	if(out_argv)
		*out_argv = argv;
	if(out_args_consumed)
		*out_args_consumed = (unsigned int)(ptrdiff_t)(argv - orig_argv);

	return result;
}
