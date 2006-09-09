#include "compare_argument.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum option_comparison_result compare_argument(const char option_name_char, const char *option_name, const char **argv, bool option_arg_optional, const char **out_option_arg) {
	if(!argv) {
		fprintf(stderr, "compare_argument: internal error: compare_argument received an argv of NULL (please tell the developer)\n");
		return option_comparison_error;
	}

	const char *arg = *argv;
	if(!arg) {
		fprintf(stderr, "compare_argument: internal error: compare_argument received an empty argv (please tell the developer)\n");
		return false;
	}

	if(*arg != '-') {
		//Not an option (options begin with -).
		return option_comparison_notanopt;
	}
	if(*++arg != '-') {
		if(*arg == '\0') {
			//Special filename for stdio (-).
			return option_comparison_stdio;
		} else if(*arg == '-') {
			if(*++arg == '\0') {
				//Options terminator (--).
				return option_comparison_endofoptions;
			} else {
				if(!option_name) {
					//We're not expecting a long option, so fail the match immediately.
					return option_comparison_nomatch;
				} else {
					//Long option (e.g. --file).
					size_t name_len = strlen(option_name);
					if(strncmp(arg, option_name, name_len) == 0) {
						//It's a match! Look for a value.
						if(!out_option_arg) {
							if(*arg == '=') {
								//The argument value follows.
								//Unfortunately, we weren't expecting an argument to this option. Return an error indicating this fact.
								return option_comparison_optargunexpected;
							} else if(*arg != '\0') {
								//Not a match! We were fooled by restricting the search to name_len characters.
								//This argument's option name is longer than that, and the option name that we're looking for is a prefix of it.
								//But they are not equal, so this is not a match.
								return option_comparison_nomatch;
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
								return option_comparison_nomatch;
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
					} else {
						//No match.
						return option_comparison_nomatch;
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
		}
	}
}
