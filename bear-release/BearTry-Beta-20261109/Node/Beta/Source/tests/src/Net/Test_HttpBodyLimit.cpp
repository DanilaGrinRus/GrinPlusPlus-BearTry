#include <catch.hpp>

#include <Net/Util/HTTPUtil.h>

TEST_CASE("HTTPUtil caps declared request body size")
{
	REQUIRE(HTTPUtil::IsRequestBodySizeAllowed(0));
	REQUIRE(HTTPUtil::IsRequestBodySizeAllowed(1));
	REQUIRE(HTTPUtil::IsRequestBodySizeAllowed(HTTPUtil::MAX_REQUEST_BODY_BYTES));
	REQUIRE_FALSE(HTTPUtil::IsRequestBodySizeAllowed(HTTPUtil::MAX_REQUEST_BODY_BYTES + 1));
	REQUIRE_FALSE(HTTPUtil::IsRequestBodySizeAllowed(16LL * 1024 * 1024));
	REQUIRE_FALSE(HTTPUtil::IsRequestBodySizeAllowed(256LL * 1024 * 1024));
	// -1 means chunked/unknown; GetRequestBody returns nullopt for it.
	REQUIRE_FALSE(HTTPUtil::IsRequestBodySizeAllowed(-1));
}
