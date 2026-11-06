#include "unity.h"
#include "services/binding_url.h"
#include <string.h>

TEST_CASE("binding URL encodes query delimiters and UTF8 bytes", "[binding]") {
    char url[BINDING_URL_CAP];
    TEST_ASSERT_TRUE(binding_url_build(url, sizeof url, "abcdef123456", "a&b=/# +密") > 0);
    TEST_ASSERT_EQUAL_STRING("https://ai-passport.folotoy.cn?s=abcdef123456&k=a%26b%3D%2F%23%20%2B%E5%AF%86", url);
}

TEST_CASE("binding URL keeps full 64 byte Key and rejects overflow", "[binding]") {
    char url[BINDING_URL_CAP], key[66];
    memset(key, '&', 64); key[64] = 0;
    int n = binding_url_build(url, sizeof url, "abcdef123456", key);
    TEST_ASSERT_TRUE(n > 192);
    TEST_ASSERT_EQUAL_STRING("%26", url + n - 3);
    TEST_ASSERT_EQUAL_INT(-1, binding_url_build(url, (size_t)n, "abcdef123456", key));
    TEST_ASSERT_EQUAL_STRING("", url);
    key[64] = '&'; key[65] = 0;
    TEST_ASSERT_EQUAL_INT(-1, binding_url_build(url, sizeof url, "abcdef123456", key));
    TEST_ASSERT_EQUAL_INT(-1, binding_url_build(url, sizeof url, "abcdef123456", ""));
    TEST_ASSERT_EQUAL_INT(-1, binding_url_build(url, sizeof url, NULL, "key"));
    TEST_ASSERT_EQUAL_INT(-1, binding_url_build(url, sizeof url, "abcdef1234567", "key"));
}
