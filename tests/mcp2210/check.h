/* SPDX-License-Identifier: MIT */
/*
 * The smallest thing that can be called a test runner, in C.
 *
 * It is the C sibling of tests/framework/check.h and exists for the same
 * reason: no gtest, no Catch2, nothing to install before the evidence can be
 * rerun. The one thing it adds is a third outcome.
 *
 * A check either passes or fails. A DEFECT is neither: it is a behaviour this
 * suite expects to be WRONG today, because the fix for it is a phase of
 * implementation_plan_mcp2210.md that has not been executed. Writing those as
 * failures would make a red suite the normal state and nobody would read it;
 * writing them as passes would be a lie. They are counted separately, they do
 * not affect the exit status, and if one of them starts behaving correctly the
 * runner says so - which is the signal that the phase landed and the check
 * should be promoted.
 */

#ifndef MCP2210_TEST_CHECK_H
#define MCP2210_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

struct check_counters {
	int checks;
	int failures;
	int defects_confirmed;
	int defects_resolved;
};

extern struct check_counters counters;

static inline void section(const char *name)
{
	printf("\n-- %s\n", name);
}

static inline void record(int passed, const char *expr, const char *file, int line,
			  const char *detail)
{
	counters.checks++;
	if (passed)
		return;
	counters.failures++;
	printf("   FALHA  %s:%d\n          %s\n", file, line, expr);
	if (detail && *detail)
		printf("          %s\n", detail);
}

static inline void record_defect(int still_broken, const char *what,
				 const char *plan_ref)
{
	if (still_broken) {
		counters.defects_confirmed++;
		printf("   DEFEITO CONFIRMADO  %s\n          %s\n", plan_ref, what);
	} else {
		counters.defects_resolved++;
		printf("   DEFEITO RESOLVIDO   %s\n          %s\n"
		       "          promova esta verificacao para CHECK\n",
		       plan_ref, what);
	}
}

static inline int report(void)
{
	printf("\n%d verificacoes, %d falhas\n", counters.checks, counters.failures);
	printf("%d defeitos confirmados, %d resolvidos\n",
	       counters.defects_confirmed, counters.defects_resolved);
	return counters.failures == 0 ? 0 : 1;
}

#define CHECK(expr)	record(!!(expr), #expr, __FILE__, __LINE__, NULL)

#define CHECK_MSG(expr, detail) \
	record(!!(expr), #expr, __FILE__, __LINE__, (detail))

#define CHECK_EQ(actual, expected)					\
	do {								\
		long _a = (long)(actual);				\
		long _b = (long)(expected);				\
		char _d[128];						\
		snprintf(_d, sizeof(_d), "esperado %ld, obtido %ld", _b, _a); \
		record(_a == _b, #actual " == " #expected,		\
		       __FILE__, __LINE__, _d);				\
	} while (0)

/*
 * `broken` is the expression that is TRUE while the defect is present.
 */
#define DEFECT(broken, what, plan_ref)	record_defect(!!(broken), what, plan_ref)

#endif /* MCP2210_TEST_CHECK_H */
