#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "../resgroup.c"

void
test__GetResGroupForTxn_when_role_id_is_invalid(void **state)
{
	will_return(GetUserId, 0x1001);

	expect_value(GetResGroupIdForRole, roleid, 0x1001);
	will_return(GetResGroupIdForRole, InvalidOid);

	assert_int_equal(GetResGroupForTxn(), DEFAULTRESGROUP_OID);
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test__GetResGroupForTxn_when_role_id_is_invalid),
	};

	run_tests(tests);
}
