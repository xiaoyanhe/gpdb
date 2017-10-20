#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "../resgroup.c"

Oid GetResGroupIdForRole(Oid user_id)
{
	check_expected(user_id);
	return (Oid)mock();
}

Oid GetUserId(void)
{
	return (Oid)mock();
}

void
test_default(void **state)
{
	will_return(GetUserId, 1001);

	expect_value(GetResGroupIdForRole, user_id, 1001);
	will_return(GetResGroupIdForRole, 6401);
	Oid actualGroupId = ResGroupAssign();
	assert_int_equal(actualGroupId, 6401);
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test_default),
	};

	run_tests(tests);
}