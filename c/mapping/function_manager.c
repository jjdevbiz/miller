#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "mapping/function_manager.h"
#include "mapping/context_flags.h"
#include "mapping/rval_evaluators.h"

// ----------------------------------------------------------------
typedef enum _func_class_t {
	FUNC_CLASS_ARITHMETIC,
	FUNC_CLASS_MATH,
	FUNC_CLASS_BOOLEAN,
	FUNC_CLASS_STRING,
	FUNC_CLASS_CONVERSION,
	FUNC_CLASS_TIME
} func_class_t;

typedef enum _arity_check_t {
	ARITY_CHECK_PASS,
	ARITY_CHECK_FAIL,
	ARITY_CHECK_NO_SUCH
} arity_check_t;

typedef struct _function_lookup_t {
	func_class_t function_class;
	char*        function_name;
	int          arity;
	int          variadic;
	char*        usage_string;
} function_lookup_t;

// This is shared between all instances
static function_lookup_t FUNCTION_LOOKUP_TABLE[];

// ----------------------------------------------------------------
// See also comments in rval_evaluators.h

static void fmgr_check_arity_with_report(fmgr_t* pfmgr, char* function_name,
	int user_provided_arity, int* pvariadic);

static rval_evaluator_t* fmgr_alloc_evaluator_from_variadic_func_name(char* function_name, rval_evaluator_t** pargs, int nargs);

static rval_evaluator_t* fmgr_alloc_evaluator_from_zary_func_name(char* function_name);

static rval_evaluator_t* fmgr_alloc_evaluator_from_unary_func_name(char* fnnm, rval_evaluator_t* parg1);

static rval_evaluator_t* fmgr_alloc_evaluator_from_binary_func_name(char* fnnm,
	rval_evaluator_t* parg1, rval_evaluator_t* parg2);

static rval_evaluator_t* fmgr_alloc_evaluator_from_binary_regex_arg2_func_name(char* fnnm,
	rval_evaluator_t* parg1, char* regex_string, int ignore_case);

static rval_evaluator_t* fmgr_alloc_evaluator_from_ternary_func_name(char* fnnm,
	rval_evaluator_t* parg1, rval_evaluator_t* parg2, rval_evaluator_t* parg3);

static rval_evaluator_t* fmgr_alloc_evaluator_from_ternary_regex_arg2_func_name(char* fnnm,
	rval_evaluator_t* parg1, char* regex_string, int ignore_case, rval_evaluator_t* parg3);

static void resolve_func_callsite(fmgr_t* pfmgr, rval_evaluator_t* pev);

// ----------------------------------------------------------------
fmgr_t* fmgr_alloc() {
	fmgr_t* pfmgr = mlr_malloc_or_die(sizeof(fmgr_t));

	pfmgr->function_lookup_table = &FUNCTION_LOOKUP_TABLE[0];

	pfmgr->built_in_function_names = hss_alloc();
	for (int i = 0; ; i++) {
		function_lookup_t* plookup = &pfmgr->function_lookup_table[i];
		char* fname = plookup->function_name;
		if (fname == NULL)
			break;
		hss_add(pfmgr->built_in_function_names, fname);
	}

	pfmgr->pudf_names_to_defsite_states = lhmsv_alloc();

	pfmgr->pfunc_callsite_evaluators_to_resolve = sllv_alloc();

	return pfmgr;
}

// ----------------------------------------------------------------
void fmgr_free(fmgr_t* pfmgr) {
	if (pfmgr == NULL)
		return;

	for (lhmsve_t* pe = pfmgr->pudf_names_to_defsite_states->phead; pe != NULL; pe = pe->pnext) {
		udf_defsite_state_t * pdefsite_state = pe->pvvalue;
		free(pdefsite_state->name);
		pdefsite_state->pfree_func(pdefsite_state->pvstate);
		free(pdefsite_state);
	}
	lhmsv_free(pfmgr->pudf_names_to_defsite_states);
	sllv_free(pfmgr->pfunc_callsite_evaluators_to_resolve);
	hss_free(pfmgr->built_in_function_names);
	free(pfmgr);
}

// ----------------------------------------------------------------
void fmgr_install_udf(fmgr_t* pfmgr, udf_defsite_state_t* pdefsite_state) {
	if (hss_has(pfmgr->built_in_function_names, pdefsite_state->name)) {
		fprintf(stderr, "%s: function named \"%s\" must not override a built-in function of the same name.\n",
			MLR_GLOBALS.bargv0, pdefsite_state->name);
		exit(1);
	}
	if (lhmsv_get(pfmgr->pudf_names_to_defsite_states, pdefsite_state->name)) {
		fprintf(stderr, "%s: function named \"%s\" has already been defined.\n",
			MLR_GLOBALS.bargv0, pdefsite_state->name);
		exit(1);
	}
	lhmsv_put(pfmgr->pudf_names_to_defsite_states, mlr_strdup_or_die(pdefsite_state->name), pdefsite_state,
		FREE_ENTRY_KEY);
}

// ================================================================
static function_lookup_t FUNCTION_LOOKUP_TABLE[] = {

	{FUNC_CLASS_ARITHMETIC, "+",  2,0, "Addition."},
	{FUNC_CLASS_ARITHMETIC, "+",  1,0, "Unary plus."},
	{FUNC_CLASS_ARITHMETIC, "-",  2,0, "Subtraction."},
	{FUNC_CLASS_ARITHMETIC, "-",  1,0, "Unary minus."},
	{FUNC_CLASS_ARITHMETIC, "*",  2,0, "Multiplication."},
	{FUNC_CLASS_ARITHMETIC, "/",  2,0, "Division."},
	{FUNC_CLASS_ARITHMETIC, "//", 2,0, "Integer division: rounds to negative (pythonic)."},
	{FUNC_CLASS_ARITHMETIC, "%",  2,0, "Remainder; never negative-valued (pythonic)."},
	{FUNC_CLASS_ARITHMETIC, "**", 2,0, "Exponentiation; same as pow, but as an infix\noperator."},
	{FUNC_CLASS_ARITHMETIC, "|",  2,0, "Bitwise OR."},
	{FUNC_CLASS_ARITHMETIC, "^",  2,0, "Bitwise XOR."},
	{FUNC_CLASS_ARITHMETIC, "&",  2,0, "Bitwise AND."},
	{FUNC_CLASS_ARITHMETIC, "~",  1,0,
		"Bitwise NOT. Beware '$y=~$x' since =~ is the\nregex-match operator: try '$y = ~$x'."},
	{FUNC_CLASS_ARITHMETIC, "<<", 2,0, "Bitwise left-shift."},
	{FUNC_CLASS_ARITHMETIC, ">>", 2,0, "Bitwise right-shift."},

	{FUNC_CLASS_BOOLEAN, "==",  2,0, "String/numeric equality. Mixing number and string\nresults in string compare."},
	{FUNC_CLASS_BOOLEAN, "!=",  2,0, "String/numeric inequality. Mixing number and string\nresults in string compare."},
	{FUNC_CLASS_BOOLEAN, "=~",  2,0,
		"String (left-hand side) matches regex (right-hand\n"
		"side), e.g. '$name =~ \"^a.*b$\"'."},
	{FUNC_CLASS_BOOLEAN, "!=~", 2,0,
		"String (left-hand side) does not match regex\n"
		"(right-hand side), e.g. '$name !=~ \"^a.*b$\"'."},
	{FUNC_CLASS_BOOLEAN, ">",   2,0,
		"String/numeric greater-than. Mixing number and string\n"
		"results in string compare."},
	{FUNC_CLASS_BOOLEAN, ">=",  2,0,
		"String/numeric greater-than-or-equals. Mixing number\n"
		"and string results in string compare."},
	{FUNC_CLASS_BOOLEAN, "<",   2,0,
		"String/numeric less-than. Mixing number and string\n"
		"results in string compare."},
	{FUNC_CLASS_BOOLEAN, "<=",  2,0,
		"String/numeric less-than-or-equals. Mixing number\n"
		"and string results in string compare."},
	{FUNC_CLASS_BOOLEAN, "&&",  2,0, "Logical AND."},
	{FUNC_CLASS_BOOLEAN, "||",  2,0, "Logical OR."},
	{FUNC_CLASS_BOOLEAN, "^^",  2,0, "Logical XOR."},
	{FUNC_CLASS_BOOLEAN, "!",   1,0, "Logical negation."},
	{FUNC_CLASS_BOOLEAN, "? :", 3,0, "Ternary operator."},

	{FUNC_CLASS_CONVERSION, "isnull",      1,0, "True if argument is null (empty or absent), false otherwise"},
	{FUNC_CLASS_CONVERSION, "isnotnull",   1,0, "False if argument is null (empty or absent), true otherwise."},
	{FUNC_CLASS_CONVERSION, "isabsent",    1,0, "False if field is present in input, false otherwise"},
	{FUNC_CLASS_CONVERSION, "ispresent",   1,0, "True if field is present in input, false otherwise."},
	{FUNC_CLASS_CONVERSION, "isempty",     1,0, "True if field is present in input with empty value, false otherwise."},
	{FUNC_CLASS_CONVERSION, "isnotempty",  1,0, "False if field is present in input with empty value, false otherwise"},
	{FUNC_CLASS_CONVERSION, "isnumeric",   1,0, "True if field is present with value inferred to be int or float"},
	{FUNC_CLASS_CONVERSION, "isint",       1,0, "True if field is present with value inferred to be int "},
	{FUNC_CLASS_CONVERSION, "isfloat",     1,0, "True if field is present with value inferred to be float"},
	{FUNC_CLASS_CONVERSION, "isbool",      1,0, "True if field is present with boolean value"},
	{FUNC_CLASS_CONVERSION, "isstring",    1,0, "True if field is present with string (including empty-string) value"},
	{FUNC_CLASS_CONVERSION, "boolean",     1,0, "Convert int/float/bool/string to boolean."},
	{FUNC_CLASS_CONVERSION, "float",       1,0, "Convert int/float/bool/string to float."},
	{FUNC_CLASS_CONVERSION, "fmtnum",    2,0,
		"Convert int/float/bool to string using\n"
		"printf-style format string, e.g. '$s = fmtnum($n, \"%06lld\")'."},
	{FUNC_CLASS_CONVERSION, "hexfmt",    1,0, "Convert int to string, e.g. 255 to \"0xff\"."},
	{FUNC_CLASS_CONVERSION, "int",       1,0, "Convert int/float/bool/string to int."},
	{FUNC_CLASS_CONVERSION, "string",    1,0, "Convert int/float/bool/string to string."},
	{FUNC_CLASS_CONVERSION, "typeof",    1,0,
		"Convert argument to type of argument (e.g.\n"
		"MT_STRING). For debug."},

	{FUNC_CLASS_STRING, ".",        2,0, "String concatenation."},
	{FUNC_CLASS_STRING, "gsub",     3,0, "Example: '$name=gsub($name, \"old\", \"new\")'\n(replace all)."},
	{FUNC_CLASS_STRING, "strlen",   1,0, "String length."},
	{FUNC_CLASS_STRING, "sub",      3,0, "Example: '$name=sub($name, \"old\", \"new\")'\n(replace once)."},
	{FUNC_CLASS_STRING, "substr",   3,0,
		"substr(s,m,n) gives substring of s from 0-up position m to n \n"
		"inclusive. Negative indices -len .. -1 alias to 0 .. len-1."},
	{FUNC_CLASS_STRING, "tolower",  1,0, "Convert string to lowercase."},
	{FUNC_CLASS_STRING, "toupper",  1,0, "Convert string to uppercase."},

	{FUNC_CLASS_MATH, "abs",      1,0, "Absolute value."},
	{FUNC_CLASS_MATH, "acos",     1,0, "Inverse trigonometric cosine."},
	{FUNC_CLASS_MATH, "acosh",    1,0, "Inverse hyperbolic cosine."},
	{FUNC_CLASS_MATH, "asin",     1,0, "Inverse trigonometric sine."},
	{FUNC_CLASS_MATH, "asinh",    1,0, "Inverse hyperbolic sine."},
	{FUNC_CLASS_MATH, "atan",     1,0, "One-argument arctangent."},
	{FUNC_CLASS_MATH, "atan2",    2,0, "Two-argument arctangent."},
	{FUNC_CLASS_MATH, "atanh",    1,0, "Inverse hyperbolic tangent."},
	{FUNC_CLASS_MATH, "cbrt",     1,0, "Cube root."},
	{FUNC_CLASS_MATH, "ceil",     1,0, "Ceiling: nearest integer at or above."},
	{FUNC_CLASS_MATH, "cos",      1,0, "Trigonometric cosine."},
	{FUNC_CLASS_MATH, "cosh",     1,0, "Hyperbolic cosine."},
	{FUNC_CLASS_MATH, "erf",      1,0, "Error function."},
	{FUNC_CLASS_MATH, "erfc",     1,0, "Complementary error function."},
	{FUNC_CLASS_MATH, "exp",      1,0, "Exponential function e**x."},
	{FUNC_CLASS_MATH, "expm1",    1,0, "e**x - 1."},
	{FUNC_CLASS_MATH, "floor",    1,0, "Floor: nearest integer at or below."},
	// See also http://johnkerl.org/doc/randuv.pdf for more about urand() -> other distributions
	{FUNC_CLASS_MATH, "invqnorm", 1,0,
		"Inverse of normal cumulative distribution\n"
		"function. Note that invqorm(urand()) is normally distributed."},
	{FUNC_CLASS_MATH, "log",      1,0, "Natural (base-e) logarithm."},
	{FUNC_CLASS_MATH, "log10",    1,0, "Base-10 logarithm."},
	{FUNC_CLASS_MATH, "log1p",    1,0, "log(1-x)."},
	{FUNC_CLASS_MATH, "logifit",  3,0, "Given m and b from logistic regression, compute\nfit: $yhat=logifit($x,$m,$b)."},
	{FUNC_CLASS_MATH, "madd",     3,0, "a + b mod m (integers)"},
	{FUNC_CLASS_MATH, "max",      0,1, "max of n numbers; null loses"},
	{FUNC_CLASS_MATH, "mexp",     3,0, "a ** b mod m (integers)"},
	{FUNC_CLASS_MATH, "min",      0,1, "Min of n numbers; null loses"},
	{FUNC_CLASS_MATH, "mmul",     3,0, "a * b mod m (integers)"},
	{FUNC_CLASS_MATH, "msub",     3,0, "a - b mod m (integers)"},
	{FUNC_CLASS_MATH, "pow",      2,0, "Exponentiation; same as **."},
	{FUNC_CLASS_MATH, "qnorm",    1,0, "Normal cumulative distribution function."},
	{FUNC_CLASS_MATH, "round",    1,0, "Round to nearest integer."},
	{FUNC_CLASS_MATH, "roundm",   2,0, "Round to nearest multiple of m: roundm($x,$m) is\nthe same as round($x/$m)*$m"},
	{FUNC_CLASS_MATH, "sgn",      1,0, "+1 for positive input, 0 for zero input, -1 for\nnegative input."},
	{FUNC_CLASS_MATH, "sin",      1,0, "Trigonometric sine."},
	{FUNC_CLASS_MATH, "sinh",     1,0, "Hyperbolic sine."},
	{FUNC_CLASS_MATH, "sqrt",     1,0, "Square root."},
	{FUNC_CLASS_MATH, "tan",      1,0, "Trigonometric tangent."},
	{FUNC_CLASS_MATH, "tanh",     1,0, "Hyperbolic tangent."},
	{FUNC_CLASS_MATH, "urand",    0,0,
		"Floating-point numbers on the unit interval.\n"
		"Int-valued example: '$n=floor(20+urand()*11)'." },
	{FUNC_CLASS_MATH, "urand32",  0,0, "Integer uniformly distributed 0 and 2**32-1\n"
	"inclusive." },
	{FUNC_CLASS_MATH, "urandint", 2,0, "Integer uniformly distributed between inclusive\ninteger endpoints." },

	{FUNC_CLASS_TIME, "dhms2fsec", 1,0,
		"Recovers floating-point seconds as in\n"
		"dhms2fsec(\"5d18h53m20.250000s\") = 500000.250000"},
	{FUNC_CLASS_TIME, "dhms2sec",  1,0, "Recovers integer seconds as in\ndhms2sec(\"5d18h53m20s\") = 500000"},
	{FUNC_CLASS_TIME, "fsec2dhms", 1,0,
		"Formats floating-point seconds as in\nfsec2dhms(500000.25) = \"5d18h53m20.250000s\""},
	{FUNC_CLASS_TIME, "fsec2hms",  1,0,
		"Formats floating-point seconds as in\nfsec2hms(5000.25) = \"01:23:20.250000\""},
	{FUNC_CLASS_TIME, "gmt2sec",   1,0, "Parses GMT timestamp as integer seconds since\nthe epoch."},
	{FUNC_CLASS_TIME, "hms2fsec",  1,0,
		"Recovers floating-point seconds as in\nhms2fsec(\"01:23:20.250000\") = 5000.250000"},
	{FUNC_CLASS_TIME, "hms2sec",   1,0, "Recovers integer seconds as in\nhms2sec(\"01:23:20\") = 5000"},
	{FUNC_CLASS_TIME, "sec2dhms",  1,0, "Formats integer seconds as in sec2dhms(500000)\n= \"5d18h53m20s\""},
	{FUNC_CLASS_TIME, "sec2gmt",   1,0,
		"Formats seconds since epoch (integer part)\n"
		"as GMT timestamp, e.g. sec2gmt(1440768801.7) = \"2015-08-28T13:33:21Z\".\n"
		"Leaves non-numbers as-is."},
	{FUNC_CLASS_TIME, "sec2gmtdate", 1,0,
		"Formats seconds since epoch (integer part)\n"
		"as GMT timestamp with year-month-date, e.g. sec2gmtdate(1440768801.7) = \"2015-08-28\".\n"
		"Leaves non-numbers as-is."},
	{FUNC_CLASS_TIME, "sec2hms",   1,0,
		"Formats integer seconds as in\n"
		"sec2hms(5000) = \"01:23:20\""},
	{FUNC_CLASS_TIME, "strftime",  2,0,
		"Formats seconds since epoch (integer part)\n"
		"as timestamp, e.g.\n"
		"strftime(1440768801.7,\"%Y-%m-%dT%H:%M:%SZ\") = \"2015-08-28T13:33:21Z\"."},
	{FUNC_CLASS_TIME, "strptime",  2,0,
		"Parses timestamp as integer seconds since epoch,\n"
		"e.g. strptime(\"2015-08-28T13:33:21Z\",\"%Y-%m-%dT%H:%M:%SZ\") = 1440768801."},
	{FUNC_CLASS_TIME, "systime",   0,0,
		"Floating-point seconds since the epoch,\n"
		"e.g. 1440768801.748936." },

	{0, NULL, -1 , -1, NULL}, // table terminator
};

// ----------------------------------------------------------------
static arity_check_t check_arity(function_lookup_t lookup_table[], char* function_name,
	int user_provided_arity, int *parity, int* pvariadic)
{
	*parity = -1;
	*pvariadic = FALSE;
	int found_function_name = FALSE;
	for (int i = 0; ; i++) {
		function_lookup_t* plookup = &lookup_table[i];
		if (plookup->function_name == NULL)
			break;
		if (streq(function_name, plookup->function_name)) {
			found_function_name = TRUE;
			*parity = plookup->arity;
			if (plookup->variadic) {
				*pvariadic = TRUE;
				return ARITY_CHECK_PASS;
			}
			if (user_provided_arity == plookup->arity) {
				return ARITY_CHECK_PASS;
			}
		}
	}
	if (found_function_name) {
		return ARITY_CHECK_FAIL;
	} else {
		return ARITY_CHECK_NO_SUCH;
	}
}

static void fmgr_check_arity_with_report(fmgr_t* pfmgr, char* function_name,
	int user_provided_arity, int* pvariadic)
{
	int arity = -1;
	arity_check_t result = check_arity(pfmgr->function_lookup_table, function_name, user_provided_arity,
		&arity, pvariadic);
	if (result == ARITY_CHECK_NO_SUCH) {
		fprintf(stderr, "%s: Function name \"%s\" not found.\n", MLR_GLOBALS.bargv0, function_name);
		exit(1);
	}
	if (result == ARITY_CHECK_FAIL) {
		// More flexibly, I'd have a list of arities supported by each
		// function. But this is overkill: there are unary and binary minus,
		// and everything else has a single arity.
		if (streq(function_name, "-")) {
			fprintf(stderr, "%s: Function named \"%s\" takes one argument or two; got %d.\n",
				MLR_GLOBALS.bargv0, function_name, user_provided_arity);
		} else {
			fprintf(stderr, "%s: Function named \"%s\" takes %d argument%s; got %d.\n",
				MLR_GLOBALS.bargv0, function_name, arity, (arity == 1) ? "" : "s", user_provided_arity);
		}
		exit(1);
	}
}

static char* function_class_to_desc(func_class_t function_class) {
	switch(function_class) {
	case FUNC_CLASS_ARITHMETIC: return "arithmetic"; break;
	case FUNC_CLASS_MATH:       return "math";       break;
	case FUNC_CLASS_BOOLEAN:    return "boolean";    break;
	case FUNC_CLASS_STRING:     return "string";     break;
	case FUNC_CLASS_CONVERSION: return "conversion"; break;
	case FUNC_CLASS_TIME:       return "time";       break;
	default:                    return "???";        break;
	}
}

void fmgr_list_functions(fmgr_t* pfmgr, FILE* output_stream, char* leader) {
	char* separator = " ";
	int leaderlen = strlen(leader);
	int separatorlen = strlen(separator);
	int linelen = leaderlen;
	int j = 0;

	for (int i = 0; ; i++) {
		function_lookup_t* plookup = &FUNCTION_LOOKUP_TABLE[i];
		char* fname = plookup->function_name;
		if (fname == NULL)
			break;
		int fnamelen = strlen(fname);
		linelen += separatorlen + fnamelen;
		if (linelen >= 80) {
			fprintf(output_stream, "\n");
			linelen = 0;
			linelen = leaderlen + separatorlen + fnamelen;
			j = 0;
		}
		if (j == 0)
			fprintf(output_stream, "%s", leader);
		fprintf(output_stream, "%s%s", separator, fname);
		j++;
	}
	fprintf(output_stream, "\n");
}

// Pass function_name == NULL to get usage for all functions.
void fmgr_function_usage(fmgr_t* pfmgr, FILE* output_stream, char* function_name) {
	int found = FALSE;
	char* nfmt = "%s (class=%s #args=%d): %s\n";
	char* vfmt = "%s (class=%s variadic): %s\n";

	for (int i = 0; ; i++) {
		function_lookup_t* plookup = &FUNCTION_LOOKUP_TABLE[i];
		if (plookup->function_name == NULL) // end of table
			break;
		if (function_name == NULL || streq(function_name, plookup->function_name)) {
			if (plookup->variadic) {
				fprintf(output_stream, vfmt, plookup->function_name,
					function_class_to_desc(plookup->function_class),
					plookup->usage_string);
			} else {
				fprintf(output_stream, nfmt, plookup->function_name,
					function_class_to_desc(plookup->function_class),
					plookup->arity, plookup->usage_string);
			}
			found = TRUE;
		}
		if (function_name == NULL)
			fprintf(output_stream, "\n");
	}
	if (!found)
		fprintf(output_stream, "%s: no such function.\n", function_name);
	if (function_name == NULL) {
		fprintf(output_stream, "To set the seed for urand, you may specify decimal or hexadecimal 32-bit\n");
		fprintf(output_stream, "numbers of the form \"%s --seed 123456789\" or \"%s --seed 0xcafefeed\".\n",
			MLR_GLOBALS.bargv0, MLR_GLOBALS.bargv0);
		fprintf(output_stream, "Miller's built-in variables are NF, NR, FNR, FILENUM, and FILENAME (awk-like)\n");
		fprintf(output_stream, "along with the mathematical constants PI and E.\n");
	}
}

void fmgr_list_all_functions_raw(fmgr_t* pfmgr, FILE* output_stream) {
	for (int i = 0; ; i++) {
		function_lookup_t* plookup = &FUNCTION_LOOKUP_TABLE[i];
		if (plookup->function_name == NULL) // end of table
			break;
		printf("%s\n", plookup->function_name);
	}
}

// ================================================================
typedef struct _rval_evaluator_udf_callsite_state_t {
	int arity;
	rval_evaluator_t** pevals;
	mv_t* args;
	udf_defsite_state_t* pdefsite_state;
} rval_evaluator_udf_callsite_state_t;

static mv_t rval_evaluator_udf_callsite_process(void* pvstate, variables_t* pvars) {
	rval_evaluator_udf_callsite_state_t* pstate = pvstate;

	for (int i = 0; i < pstate->arity; i++) {
		pstate->args[i] = pstate->pevals[i]->pprocess_func(pstate->pevals[i]->pvstate, pvars);
	}

	return pstate->pdefsite_state->pprocess_func(pstate->pdefsite_state->pvstate,
		pstate->arity, pstate->args, pvars);
}

static void rval_evaluator_udf_callsite_free(rval_evaluator_t* pevaluator) {
	rval_evaluator_udf_callsite_state_t* pstate = pevaluator->pvstate;
	for (int i = 0; i < pstate->arity; i++) {
		rval_evaluator_t* peval = pstate->pevals[i];
		peval->pfree_func(peval);
		mv_free(&pstate->args[i]);
	}
	free(pstate->pevals);
	free(pstate->args);
	free(pstate);
	free(pevaluator);
}

static rval_evaluator_t* fmgr_alloc_from_udf_callsite(fmgr_t* pfmgr, udf_defsite_state_t* pdefsite_state,
	mlr_dsl_ast_node_t* pnode, char* function_name, int arity, int type_inferencing, int context_flags)
{
	rval_evaluator_t* pudf_callsite_evaluator = mlr_malloc_or_die(sizeof(rval_evaluator_t));
	rval_evaluator_udf_callsite_state_t* pstate = mlr_malloc_or_die(sizeof(rval_evaluator_udf_callsite_state_t));

	pstate->arity = pnode->pchildren->length;

	pstate->pevals = mlr_malloc_or_die(pstate->arity * sizeof(rval_evaluator_t*));
	int i = 0;
	for (sllve_t* pe = pnode->pchildren->phead; pe != NULL; pe = pe->pnext, i++) {
		mlr_dsl_ast_node_t* parg_node = pe->pvvalue;
		pstate->pevals[i] = rval_evaluator_alloc_from_ast(parg_node,
			pfmgr, type_inferencing, context_flags);
	}

	pstate->args = mlr_malloc_or_die(pstate->arity * sizeof(mv_t));
	for (i = 0; i < pstate->arity; i++) {
		pstate->args[i] = mv_absent();
	}

	pstate->pdefsite_state = pdefsite_state;

	pudf_callsite_evaluator->pvstate = pstate;
	pudf_callsite_evaluator->pprocess_func = rval_evaluator_udf_callsite_process;
	pudf_callsite_evaluator->pfree_func = rval_evaluator_udf_callsite_free;

	return pudf_callsite_evaluator;
}

// ================================================================
typedef struct _unresolved_func_callsite_state_t {
	char* function_name;
	int arity;
	int type_inferencing;
	int context_flags;
	mlr_dsl_ast_node_t* pnode;
} unresolved_func_callsite_state_t;

static unresolved_func_callsite_state_t* unresolved_callsite_alloc(char* function_name, int arity,
	int type_inferencing, int context_flags, mlr_dsl_ast_node_t* pnode)
{
	unresolved_func_callsite_state_t* pstate = mlr_malloc_or_die(sizeof(unresolved_func_callsite_state_t));
	pstate->function_name    = mlr_strdup_or_die(function_name);
	pstate->arity            = arity;
	pstate->type_inferencing = type_inferencing;
	pstate->context_flags    = context_flags;
	pstate->pnode            = pnode;
	return pstate;
}

static void unresolved_callsite_free(unresolved_func_callsite_state_t* pstate) {
	if (pstate == NULL)
		return;
	free(pstate->function_name);
	free(pstate);
}

// ----------------------------------------------------------------
rval_evaluator_t* fmgr_alloc_from_operator_or_function_call(fmgr_t* pfmgr, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	char* function_name = pnode->text;
	int user_provided_arity = pnode->pchildren->length;

	unresolved_func_callsite_state_t* pstate = unresolved_callsite_alloc(function_name, user_provided_arity,
		type_inferencing, context_flags, pnode);

	rval_evaluator_t* pev = mlr_malloc_or_die(sizeof(rval_evaluator_t));
	pev->pvstate       = pstate;
	pev->pprocess_func = NULL;
	pev->pfree_func    = NULL;

	// Remember this callsite to a function which may or may not have been defined yet.
	// Then later we can resolve them to point to UDF bodies which have been defined.
	sllv_append(pfmgr->pfunc_callsite_evaluators_to_resolve, pev);

	return pev;
}

// ----------------------------------------------------------------
void fmgr_resolve_func_callsites(fmgr_t* pfmgr) {
	while (pfmgr->pfunc_callsite_evaluators_to_resolve->phead != NULL) {
		rval_evaluator_t* pev = sllv_pop(pfmgr->pfunc_callsite_evaluators_to_resolve);
		unresolved_func_callsite_state_t* ptemp_state = pev->pvstate;
		resolve_func_callsite(pfmgr, pev);
		unresolved_callsite_free(ptemp_state);
	}
}

static void resolve_func_callsite(fmgr_t* pfmgr, rval_evaluator_t* pev) {
	unresolved_func_callsite_state_t* pstate = pev->pvstate;
	char* function_name       = pstate->function_name;
	int   user_provided_arity = pstate->arity;
	int   type_inferencing    = pstate->type_inferencing;
	int   context_flags       = pstate->context_flags;
	mlr_dsl_ast_node_t* pnode = pstate->pnode;

	udf_defsite_state_t* pudf_defsite_state = lhmsv_get(pfmgr->pudf_names_to_defsite_states, pstate->function_name);
	if (pudf_defsite_state != NULL) {
		int udf_arity = pudf_defsite_state->arity;
		if (user_provided_arity != udf_arity) {
			fprintf(stderr, "Function named \"%s\" takes %d argument%s; got %d.\n",
				function_name, udf_arity, (udf_arity == 1) ? "" : "s", user_provided_arity);
			exit(1);
		}

		rval_evaluator_t* pcallsite_evaluator = fmgr_alloc_from_udf_callsite(pfmgr, pudf_defsite_state,
			pnode, function_name, user_provided_arity, type_inferencing, context_flags);

		*pev = *pcallsite_evaluator;
		free(pcallsite_evaluator); // xxx comment
		return;
	}

	int variadic = FALSE;
	fmgr_check_arity_with_report(pfmgr, function_name, user_provided_arity, &variadic);

	rval_evaluator_t* pevaluator = NULL;
	if (variadic) {
		int nargs = pnode->pchildren->length;
		rval_evaluator_t** pargs = mlr_malloc_or_die(nargs * sizeof(rval_evaluator_t*));
		int i = 0;
		for (sllve_t* pe = pnode->pchildren->phead; pe != NULL; pe = pe->pnext, i++) {
			mlr_dsl_ast_node_t* pchild = pe->pvvalue;
			pargs[i] = rval_evaluator_alloc_from_ast(pchild, pfmgr, type_inferencing, context_flags);
		}
		pevaluator = fmgr_alloc_evaluator_from_variadic_func_name(function_name, pargs, nargs);

	} else if (user_provided_arity == 0) {
		pevaluator = fmgr_alloc_evaluator_from_zary_func_name(function_name);
	} else if (user_provided_arity == 1) {
		mlr_dsl_ast_node_t* parg1_node = pnode->pchildren->phead->pvvalue;
		rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
		pevaluator = fmgr_alloc_evaluator_from_unary_func_name(function_name, parg1);
	} else if (user_provided_arity == 2) {
		mlr_dsl_ast_node_t* parg1_node = pnode->pchildren->phead->pvvalue;
		mlr_dsl_ast_node_t* parg2_node = pnode->pchildren->phead->pnext->pvvalue;
		int type2 = parg2_node->type;

		if ((streq(function_name, "=~") || streq(function_name, "!=~")) && type2 == MD_AST_NODE_TYPE_STRNUM_LITERAL) {
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_binary_regex_arg2_func_name(function_name,
				parg1, parg2_node->text, FALSE);
		} else if ((streq(function_name, "=~") || streq(function_name, "!=~")) && type2 == MD_AST_NODE_TYPE_REGEXI) {
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_binary_regex_arg2_func_name(function_name, parg1, parg2_node->text,
				TYPE_INFER_STRING_FLOAT_INT);
		} else {
			// regexes can still be applied here, e.g. if the 2nd argument is a non-terminal AST: however
			// the regexes will be compiled record-by-record rather than once at alloc time, which will
			// be slower.
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			rval_evaluator_t* parg2 = rval_evaluator_alloc_from_ast(parg2_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_binary_func_name(function_name, parg1, parg2);
		}

	} else if (user_provided_arity == 3) {
		mlr_dsl_ast_node_t* parg1_node = pnode->pchildren->phead->pvvalue;
		mlr_dsl_ast_node_t* parg2_node = pnode->pchildren->phead->pnext->pvvalue;
		mlr_dsl_ast_node_t* parg3_node = pnode->pchildren->phead->pnext->pnext->pvvalue;
		int type2 = parg2_node->type;

		if ((streq(function_name, "sub") || streq(function_name, "gsub")) && type2 == MD_AST_NODE_TYPE_STRNUM_LITERAL) {
			// sub/gsub-regex special case:
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			rval_evaluator_t* parg3 = rval_evaluator_alloc_from_ast(parg3_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_ternary_regex_arg2_func_name(function_name, parg1, parg2_node->text,
				FALSE, parg3);

		} else if ((streq(function_name, "sub") || streq(function_name, "gsub")) && type2 == MD_AST_NODE_TYPE_REGEXI) {
			// sub/gsub-regex special case:
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			rval_evaluator_t* parg3 = rval_evaluator_alloc_from_ast(parg3_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_ternary_regex_arg2_func_name(function_name, parg1, parg2_node->text,
				TYPE_INFER_STRING_FLOAT_INT, parg3);

		} else {
			// regexes can still be applied here, e.g. if the 2nd argument is a non-terminal AST: however
			// the regexes will be compiled record-by-record rather than once at alloc time, which will
			// be slower.
			rval_evaluator_t* parg1 = rval_evaluator_alloc_from_ast(parg1_node, pfmgr, type_inferencing, context_flags);
			rval_evaluator_t* parg2 = rval_evaluator_alloc_from_ast(parg2_node, pfmgr, type_inferencing, context_flags);
			rval_evaluator_t* parg3 = rval_evaluator_alloc_from_ast(parg3_node, pfmgr, type_inferencing, context_flags);
			pevaluator = fmgr_alloc_evaluator_from_ternary_func_name(function_name, parg1, parg2, parg3);
		}
	} else {
		fprintf(stderr, "Miller: internal coding error:  arity for function name \"%s\" misdetected.\n",
			function_name);
		exit(1);
	}
	if (pevaluator == NULL) {
		fprintf(stderr, "Miller: unrecognized function name \"%s\".\n", function_name);
		exit(1);
	}

	*pev = *pevaluator;
	free(pevaluator); // xxx comment
}

// ================================================================
static rval_evaluator_t* fmgr_alloc_evaluator_from_variadic_func_name(char* fnnm, rval_evaluator_t** pargs, int nargs) {
	if        (streq(fnnm, "min")) { return rval_evaluator_alloc_from_variadic_func(variadic_min_func, pargs, nargs);
	} else if (streq(fnnm, "max")) { return rval_evaluator_alloc_from_variadic_func(variadic_max_func, pargs, nargs);
	} else return NULL;
}

// ================================================================
static rval_evaluator_t* fmgr_alloc_evaluator_from_zary_func_name(char* function_name) {
	if        (streq(function_name, "urand")) {
		return rval_evaluator_alloc_from_x_z_func(f_z_urand_func);
	} else if (streq(function_name, "urand32")) {
		return rval_evaluator_alloc_from_x_z_func(i_z_urand32_func);
	} else if (streq(function_name, "systime")) {
		return rval_evaluator_alloc_from_x_z_func(f_z_systime_func);
	} else  {
		return NULL;
	}
}

// ================================================================
static rval_evaluator_t* fmgr_alloc_evaluator_from_unary_func_name(char* fnnm, rval_evaluator_t* parg1)  {
	if        (streq(fnnm, "!"))           { return rval_evaluator_alloc_from_b_b_func(b_b_not_func,         parg1);
	} else if (streq(fnnm, "+"))           { return rval_evaluator_alloc_from_x_x_func(x_x_upos_func,        parg1);
	} else if (streq(fnnm, "-"))           { return rval_evaluator_alloc_from_x_x_func(x_x_uneg_func,        parg1);
	} else if (streq(fnnm, "abs"))         { return rval_evaluator_alloc_from_x_x_func(x_x_abs_func,         parg1);
	} else if (streq(fnnm, "acos"))        { return rval_evaluator_alloc_from_f_f_func(f_f_acos_func,        parg1);
	} else if (streq(fnnm, "acosh"))       { return rval_evaluator_alloc_from_f_f_func(f_f_acosh_func,       parg1);
	} else if (streq(fnnm, "asin"))        { return rval_evaluator_alloc_from_f_f_func(f_f_asin_func,        parg1);
	} else if (streq(fnnm, "asinh"))       { return rval_evaluator_alloc_from_f_f_func(f_f_asinh_func,       parg1);
	} else if (streq(fnnm, "atan"))        { return rval_evaluator_alloc_from_f_f_func(f_f_atan_func,        parg1);
	} else if (streq(fnnm, "atanh"))       { return rval_evaluator_alloc_from_f_f_func(f_f_atanh_func,       parg1);
	} else if (streq(fnnm, "boolean"))     { return rval_evaluator_alloc_from_x_x_func(b_x_boolean_func,     parg1);
	} else if (streq(fnnm, "boolean"))     { return rval_evaluator_alloc_from_x_x_func(b_x_boolean_func,     parg1);
	} else if (streq(fnnm, "cbrt"))        { return rval_evaluator_alloc_from_f_f_func(f_f_cbrt_func,        parg1);
	} else if (streq(fnnm, "ceil"))        { return rval_evaluator_alloc_from_x_x_func(x_x_ceil_func,        parg1);
	} else if (streq(fnnm, "cos"))         { return rval_evaluator_alloc_from_f_f_func(f_f_cos_func,         parg1);
	} else if (streq(fnnm, "cosh"))        { return rval_evaluator_alloc_from_f_f_func(f_f_cosh_func,        parg1);
	} else if (streq(fnnm, "dhms2fsec"))   { return rval_evaluator_alloc_from_f_s_func(f_s_dhms2fsec_func,   parg1);
	} else if (streq(fnnm, "dhms2sec"))    { return rval_evaluator_alloc_from_f_s_func(i_s_dhms2sec_func,    parg1);
	} else if (streq(fnnm, "erf"))         { return rval_evaluator_alloc_from_f_f_func(f_f_erf_func,         parg1);
	} else if (streq(fnnm, "erfc"))        { return rval_evaluator_alloc_from_f_f_func(f_f_erfc_func,        parg1);
	} else if (streq(fnnm, "exp"))         { return rval_evaluator_alloc_from_f_f_func(f_f_exp_func,         parg1);
	} else if (streq(fnnm, "expm1"))       { return rval_evaluator_alloc_from_f_f_func(f_f_expm1_func,       parg1);
	} else if (streq(fnnm, "float"))       { return rval_evaluator_alloc_from_x_x_func(f_x_float_func,       parg1);
	} else if (streq(fnnm, "floor"))       { return rval_evaluator_alloc_from_x_x_func(x_x_floor_func,       parg1);
	} else if (streq(fnnm, "fsec2dhms"))   { return rval_evaluator_alloc_from_s_f_func(s_f_fsec2dhms_func,   parg1);
	} else if (streq(fnnm, "fsec2hms"))    { return rval_evaluator_alloc_from_s_f_func(s_f_fsec2hms_func,    parg1);
	} else if (streq(fnnm, "gmt2sec"))     { return rval_evaluator_alloc_from_i_s_func(i_s_gmt2sec_func,     parg1);
	} else if (streq(fnnm, "hexfmt"))      { return rval_evaluator_alloc_from_x_x_func(s_x_hexfmt_func,      parg1);
	} else if (streq(fnnm, "hms2fsec"))    { return rval_evaluator_alloc_from_f_s_func(f_s_hms2fsec_func,    parg1);
	} else if (streq(fnnm, "hms2sec"))     { return rval_evaluator_alloc_from_f_s_func(i_s_hms2sec_func,     parg1);
	} else if (streq(fnnm, "int"))         { return rval_evaluator_alloc_from_x_x_func(i_x_int_func,         parg1);
	} else if (streq(fnnm, "invqnorm"))    { return rval_evaluator_alloc_from_f_f_func(f_f_invqnorm_func,    parg1);
	} else if (streq(fnnm, "isabsent"))    { return rval_evaluator_alloc_from_x_x_func(b_x_isabsent_func,    parg1);
	} else if (streq(fnnm, "isempty"))     { return rval_evaluator_alloc_from_x_x_func(b_x_isempty_func,     parg1);
	} else if (streq(fnnm, "isnotempty"))  { return rval_evaluator_alloc_from_x_x_func(b_x_isnotempty_func,  parg1);
	} else if (streq(fnnm, "isnotnull"))   { return rval_evaluator_alloc_from_x_x_func(b_x_isnotnull_func,   parg1);
	} else if (streq(fnnm, "isnull"))      { return rval_evaluator_alloc_from_x_x_func(b_x_isnull_func,      parg1);
	} else if (streq(fnnm, "ispresent"))   { return rval_evaluator_alloc_from_x_x_func(b_x_ispresent_func,   parg1);
	} else if (streq(fnnm, "isnumeric"))   { return rval_evaluator_alloc_from_x_x_func(b_x_isnumeric_func,   parg1);
	} else if (streq(fnnm, "isint"))       { return rval_evaluator_alloc_from_x_x_func(b_x_isint_func,       parg1);
	} else if (streq(fnnm, "isfloat"))     { return rval_evaluator_alloc_from_x_x_func(b_x_isfloat_func,     parg1);
	} else if (streq(fnnm, "isbool"))      { return rval_evaluator_alloc_from_x_x_func(b_x_isbool_func,      parg1);
	} else if (streq(fnnm, "isstring"))    { return rval_evaluator_alloc_from_x_x_func(b_x_isstring_func,    parg1);
	} else if (streq(fnnm, "log"))         { return rval_evaluator_alloc_from_f_f_func(f_f_log_func,         parg1);
	} else if (streq(fnnm, "log10"))       { return rval_evaluator_alloc_from_f_f_func(f_f_log10_func,       parg1);
	} else if (streq(fnnm, "log1p"))       { return rval_evaluator_alloc_from_f_f_func(f_f_log1p_func,       parg1);
	} else if (streq(fnnm, "qnorm"))       { return rval_evaluator_alloc_from_f_f_func(f_f_qnorm_func,       parg1);
	} else if (streq(fnnm, "round"))       { return rval_evaluator_alloc_from_x_x_func(x_x_round_func,       parg1);
	} else if (streq(fnnm, "sec2dhms"))    { return rval_evaluator_alloc_from_s_i_func(s_i_sec2dhms_func,    parg1);
	} else if (streq(fnnm, "sec2gmt"))     { return rval_evaluator_alloc_from_x_x_func(s_x_sec2gmt_func,     parg1);
	} else if (streq(fnnm, "sec2gmtdate")) { return rval_evaluator_alloc_from_x_x_func(s_x_sec2gmtdate_func, parg1);
	} else if (streq(fnnm, "sec2hms"))     { return rval_evaluator_alloc_from_s_i_func(s_i_sec2hms_func,     parg1);
	} else if (streq(fnnm, "sgn"))         { return rval_evaluator_alloc_from_x_x_func(x_x_sgn_func,         parg1);
	} else if (streq(fnnm, "sin"))         { return rval_evaluator_alloc_from_f_f_func(f_f_sin_func,         parg1);
	} else if (streq(fnnm, "sinh"))        { return rval_evaluator_alloc_from_f_f_func(f_f_sinh_func,        parg1);
	} else if (streq(fnnm, "sqrt"))        { return rval_evaluator_alloc_from_f_f_func(f_f_sqrt_func,        parg1);
	} else if (streq(fnnm, "string"))      { return rval_evaluator_alloc_from_x_x_func(s_x_string_func,      parg1);
	} else if (streq(fnnm, "strlen"))      { return rval_evaluator_alloc_from_i_s_func(i_s_strlen_func,      parg1);
	} else if (streq(fnnm, "tan"))         { return rval_evaluator_alloc_from_f_f_func(f_f_tan_func,         parg1);
	} else if (streq(fnnm, "tanh"))        { return rval_evaluator_alloc_from_f_f_func(f_f_tanh_func,        parg1);
	} else if (streq(fnnm, "tolower"))     { return rval_evaluator_alloc_from_s_s_func(s_s_tolower_func,     parg1);
	} else if (streq(fnnm, "toupper"))     { return rval_evaluator_alloc_from_s_s_func(s_s_toupper_func,     parg1);
	} else if (streq(fnnm, "typeof"))      { return rval_evaluator_alloc_from_x_x_func(s_x_typeof_func,      parg1);
	} else if (streq(fnnm, "~"))           { return rval_evaluator_alloc_from_i_i_func(i_i_bitwise_not_func, parg1);
	} else return NULL;
}

// ================================================================
static rval_evaluator_t* fmgr_alloc_evaluator_from_binary_func_name(char* fnnm,
	rval_evaluator_t* parg1, rval_evaluator_t* parg2)
{
	if        (streq(fnnm, "&&"))   { return rval_evaluator_alloc_from_b_bb_and_func(parg1, parg2);
	} else if (streq(fnnm, "||"))   { return rval_evaluator_alloc_from_b_bb_or_func (parg1, parg2);
	} else if (streq(fnnm, "^^"))   { return rval_evaluator_alloc_from_b_bb_xor_func(parg1, parg2);
	} else if (streq(fnnm, "=~"))   { return rval_evaluator_alloc_from_x_ssc_func(
		matches_no_precomp_func, parg1, parg2);
	} else if (streq(fnnm, "!=~"))  { return rval_evaluator_alloc_from_x_ssc_func(does_not_match_no_precomp_func, parg1, parg2);
	} else if (streq(fnnm, "=="))   { return rval_evaluator_alloc_from_x_xx_func(eq_op_func,             parg1, parg2);
	} else if (streq(fnnm, "!="))   { return rval_evaluator_alloc_from_x_xx_func(ne_op_func,             parg1, parg2);
	} else if (streq(fnnm, ">"))    { return rval_evaluator_alloc_from_x_xx_func(gt_op_func,             parg1, parg2);
	} else if (streq(fnnm, ">="))   { return rval_evaluator_alloc_from_x_xx_func(ge_op_func,             parg1, parg2);
	} else if (streq(fnnm, "<"))    { return rval_evaluator_alloc_from_x_xx_func(lt_op_func,             parg1, parg2);
	} else if (streq(fnnm, "<="))   { return rval_evaluator_alloc_from_x_xx_func(le_op_func,             parg1, parg2);
	} else if (streq(fnnm, "."))    { return rval_evaluator_alloc_from_x_xx_func(s_xx_dot_func,          parg1, parg2);
	} else if (streq(fnnm, "+"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_plus_func,         parg1, parg2);
	} else if (streq(fnnm, "-"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_minus_func,        parg1, parg2);
	} else if (streq(fnnm, "*"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_times_func,        parg1, parg2);
	} else if (streq(fnnm, "/"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_divide_func,       parg1, parg2);
	} else if (streq(fnnm, "//"))   { return rval_evaluator_alloc_from_x_xx_func(x_xx_int_divide_func,   parg1, parg2);
	} else if (streq(fnnm, "%"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_mod_func,          parg1, parg2);
	} else if (streq(fnnm, "**"))   { return rval_evaluator_alloc_from_f_ff_func(f_ff_pow_func,          parg1, parg2);
	} else if (streq(fnnm, "pow"))  { return rval_evaluator_alloc_from_f_ff_func(f_ff_pow_func,          parg1, parg2);
	} else if (streq(fnnm, "atan2")){ return rval_evaluator_alloc_from_f_ff_func(f_ff_atan2_func,        parg1, parg2);
	} else if (streq(fnnm, "roundm")) { return rval_evaluator_alloc_from_x_xx_func(x_xx_roundm_func,     parg1, parg2);
	} else if (streq(fnnm, "fmtnum")) { return rval_evaluator_alloc_from_s_xs_func(s_xs_fmtnum_func,     parg1, parg2);
	} else if (streq(fnnm, "urandint")) { return rval_evaluator_alloc_from_i_ii_func(i_ii_urandint_func, parg1, parg2);
	} else if (streq(fnnm, "&"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_band_func,         parg1, parg2);
	} else if (streq(fnnm, "|"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_bor_func,          parg1, parg2);
	} else if (streq(fnnm, "^"))    { return rval_evaluator_alloc_from_x_xx_func(x_xx_bxor_func,         parg1, parg2);
	} else if (streq(fnnm, "<<"))   { return rval_evaluator_alloc_from_i_ii_func(i_ii_bitwise_lsh_func,  parg1, parg2);
	} else if (streq(fnnm, ">>"))   { return rval_evaluator_alloc_from_i_ii_func(i_ii_bitwise_rsh_func,  parg1, parg2);
	} else if (streq(fnnm, "strftime")) { return rval_evaluator_alloc_from_x_ns_func(s_ns_strftime_func, parg1, parg2);
	} else if (streq(fnnm, "strptime")) { return rval_evaluator_alloc_from_x_ss_func(i_ss_strptime_func, parg1, parg2);
	} else  { return NULL; }
}

static rval_evaluator_t* fmgr_alloc_evaluator_from_binary_regex_arg2_func_name(char* fnnm,
	rval_evaluator_t* parg1, char* regex_string, int ignore_case)
{
	if        (streq(fnnm, "=~"))  {
		return rval_evaluator_alloc_from_x_sr_func(matches_precomp_func,        parg1, regex_string, ignore_case);
	} else if (streq(fnnm, "!=~")) {
		return rval_evaluator_alloc_from_x_sr_func(does_not_match_precomp_func, parg1, regex_string, ignore_case);
	} else  { return NULL; }
}

// ================================================================
static rval_evaluator_t* fmgr_alloc_evaluator_from_ternary_func_name(char* fnnm,
	rval_evaluator_t* parg1, rval_evaluator_t* parg2, rval_evaluator_t* parg3)
{
	if (streq(fnnm, "sub")) {
		return rval_evaluator_alloc_from_s_sss_func(sub_no_precomp_func,  parg1, parg2, parg3);
	} else if (streq(fnnm, "gsub")) {
		return rval_evaluator_alloc_from_s_sss_func(gsub_no_precomp_func, parg1, parg2, parg3);
	} else if (streq(fnnm, "logifit")) {
		return rval_evaluator_alloc_from_f_fff_func(f_fff_logifit_func,   parg1, parg2, parg3);
	} else if (streq(fnnm, "madd")) {
		return rval_evaluator_alloc_from_i_iii_func(i_iii_modadd_func,    parg1, parg2, parg3);
	} else if (streq(fnnm, "msub")) {
		return rval_evaluator_alloc_from_i_iii_func(i_iii_modsub_func,    parg1, parg2, parg3);
	} else if (streq(fnnm, "mmul")) {
		return rval_evaluator_alloc_from_i_iii_func(i_iii_modmul_func,    parg1, parg2, parg3);
	} else if (streq(fnnm, "mexp")) {
		return rval_evaluator_alloc_from_i_iii_func(i_iii_modexp_func,    parg1, parg2, parg3);
	} else if (streq(fnnm, "substr")) {
		return rval_evaluator_alloc_from_s_sii_func(s_sii_substr_func,    parg1, parg2, parg3);
	} else if (streq(fnnm, "? :")) {
		return rval_evaluator_alloc_from_ternop(parg1, parg2, parg3);
	} else  { return NULL; }
}

static rval_evaluator_t* fmgr_alloc_evaluator_from_ternary_regex_arg2_func_name(char* fnnm,
	rval_evaluator_t* parg1, char* regex_string, int ignore_case, rval_evaluator_t* parg3)
{
	if (streq(fnnm, "sub"))  {
		return rval_evaluator_alloc_from_x_srs_func(sub_precomp_func,  parg1, regex_string, ignore_case, parg3);
	} else if (streq(fnnm, "gsub"))  {
		return rval_evaluator_alloc_from_x_srs_func(gsub_precomp_func, parg1, regex_string, ignore_case, parg3);
	} else  { return NULL; }
}
