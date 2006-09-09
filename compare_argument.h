#include <stdbool.h>

enum option_comparison_result {
	option_comparison_optargunexpected = -3, //User said --file=foo.txt, but you weren't expecting an argument to --file
	option_comparison_notanopt = -2,
	option_comparison_error = -1,
	option_comparison_nomatch,
	option_comparison_shortopt,    //-f
	option_comparison_longopt,     //--file
	option_comparison_stdio,       //-
	option_comparison_endofoptions //--
};

//option_name_char can be 0, or option_name can be NULL. Doing both will result in option_comparison_error.
enum option_comparison_result compare_argument(const char option_name_char, const char *option_name, const char **argv, bool option_arg_optional, const char **out_option_arg);
