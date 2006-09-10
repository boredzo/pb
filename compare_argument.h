#include <stdbool.h>

enum option_comparison_result {
	option_comparison_optargunexpected = -3, //User said --file=foo.txt, but you weren't expecting an argument to --file
	option_comparison_notanopt = -2,
	option_comparison_error = -1,
	option_comparison_nomatch,
	//The following results indicate that a flag value was matched.
	option_comparison_shortopt,    //-f
	option_comparison_longopt,     //--file
	option_comparison_eitheropt,   //-f or --file (bit mask: short | long)
	option_comparison_stdio,       //-
	option_comparison_endofoptions //--
};

/*
 *option_arg_optional    out_option_arg    Result: Option argument is…
 *false                  non-NULL          required
 *true                   non-NULL          optional
 *(whatever)             NULL              forbidden
 */
enum option_comparison_result compare_argument(const char option_name_char, const char *option_name, const char **argv, const char ***out_argv, bool option_arg_optional, const char **out_option_arg);
