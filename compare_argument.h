#include <stdbool.h>

enum option_comparison_result {
	option_comparison_optargunexpected = -3 << 16, //User said --file=foo.txt, but you weren't expecting an argument to --file
	option_comparison_notanopt = -2 << 16,
	option_comparison_error = -1 << 16,
	option_comparison_nomatch = 0,
	//The following results indicate that a flag value was matched.
	option_comparison_shortopt,  //-f
	option_comparison_longopt,   //--file
	option_comparison_eitheropt, //-f or --file (bit mask: short | long)
	option_comparison_stdio,     //-
	option_comparison_endofflags //--
};

/*
 *option_arg_optional    out_option_arg    Result: Option argument is…
 *false                  non-NULL          required
 *true                   non-NULL          optional
 *(whatever)             NULL              forbidden
 *
 *You should pass argv such that the argument to compare against the option names is argv[0].
 *If an option argument (an argument to the desired option) is found, it will be either after the = in argv[0] (--file=foo.txt) or it will be argv[1] (--file foo.txt).
 *
 *If option_arg_optional is true and argv[1] starts with '-', then argv[1] is not an option argument; no option argument is returned (*out_option_arg is NULL).
 *If out_option_arg is NULL, then an option argument is not expected and will not be consumed if present.
 *
 *If out_argv is not NULL, then *out_argv will be set to the sub-array of argv that is after both the option name and the option argument (if any). For example, if the option argument is in argv[1] (see above), then *out_argv will be set to &argv[2].
 *If out_args_consumed is not NULL, then *out_args_consumed will be set to the number of args consumed. For example, if argv[0] is a match for --file and argv[1] is consumed as the option argument, then *out_args_consumed will be 2.
 */
enum option_comparison_result compare_argument(const char option_name_char, const char *option_name, const char **argv, const char ***out_argv, unsigned *out_args_consumed, bool option_arg_optional, const char **out_option_arg);
