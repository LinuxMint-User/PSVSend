/* recv_path 主机单测：不依赖 VitaSDK，直接 gcc/ctest 运行。 */
#include "proto/recv_path.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static void expect_ok(const char *in, const char *want, int want_depth,
                      bool want_trunc)
{
    char out[RP_REL_MAX + 1];
    int depth = -1;
    bool trunc = false;
    RpError e = rp_parse_relpath(in, out, sizeof out, &depth, &trunc);
    if (e != RP_OK) {
        printf("FAIL parse(%s): got %s, want ok\n", in, rp_strerror(e));
        g_fail++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("FAIL parse(%s): got '%s', want '%s'\n", in, out, want);
        g_fail++;
    }
    if (depth != want_depth) {
        printf("FAIL parse(%s): depth %d, want %d\n", in, depth, want_depth);
        g_fail++;
    }
    if (trunc != want_trunc) {
        printf("FAIL parse(%s): trunc %d, want %d\n", in, trunc, want_trunc);
        g_fail++;
    }
}

static void expect_err(const char *in, RpError want)
{
    char out[RP_REL_MAX + 1];
    int depth = -1;
    bool trunc = false;
    RpError e = rp_parse_relpath(in, out, sizeof out, &depth, &trunc);
    if (e != want) {
        printf("FAIL parse(%s): got %s, want %s\n", in, rp_strerror(e),
               rp_strerror(want));
        g_fail++;
    }
    if (out[0]) {
        printf("FAIL parse(%s): output not cleared: '%s'\n", in, out);
        g_fail++;
    }
}

static void test_utf8(void)
{
    const char emoji[] = "\xF0\x9F\x98\x80";
    CHECK(rp_utf8_valid(""));
    CHECK(rp_utf8_valid("abc"));
    CHECK(rp_utf8_valid("照片"));
    CHECK(rp_utf8_valid(emoji));
    CHECK(!rp_utf8_valid("\x80"));
    CHECK(!rp_utf8_valid("\xC0\xAF"));
    CHECK(!rp_utf8_valid("\xE0\x80\xAF"));
    CHECK(!rp_utf8_valid("\xED\xA0\x80"));
    CHECK(!rp_utf8_valid("\xF4\x90\x80\x80"));
    CHECK(!rp_utf8_valid("\xE2\x82"));
    CHECK(!rp_utf8_valid("a\xFF" "b"));

    {
        const char *s = "ab\xE2\x82\xAC";   /* a b € (5 bytes) */
        CHECK(rp_utf8_floor(s, 5) == 5);
        CHECK(rp_utf8_floor(s, 4) == 2);
        CHECK(rp_utf8_floor(s, 3) == 2);
        CHECK(rp_utf8_floor(s, 2) == 2);
        CHECK(rp_utf8_floor(s, 1) == 1);
        CHECK(rp_utf8_floor(s, 0) == 0);
    }
}

static void test_parse_ok(void)
{
    expect_ok("a.jpg", "a.jpg", 1, false);
    expect_ok("Photos/2026/a.jpg", "Photos/2026/a.jpg", 3, false);
    expect_ok("Photos\\2026\\a.jpg", "Photos/2026/a.jpg", 3, false);
    expect_ok("照片/2026/旅拍.jpg", "照片/2026/旅拍.jpg", 3, false);
    expect_ok(" a / b ", "a/b", 2, false);
    expect_ok("ab:c", "ab_c", 1, false);
    expect_ok("1:2", "1_2", 1, false);
    expect_ok("a. ", "a", 1, false);
    expect_ok("???", "unnamed", 1, false);
    expect_ok("ab:c*d?e\"f<g>h|i", "ab_c_d_e_f_g_h_i", 1, false);
}

static void test_parse_err(void)
{
    expect_err("", RP_E_EMPTY);
    expect_err("/a", RP_E_ABSOLUTE);
    expect_err("//server/share", RP_E_ABSOLUTE);
    expect_err("C:\\x", RP_E_DRIVE);
    expect_err("C:foo", RP_E_DRIVE);
    expect_err("c:/foo", RP_E_DRIVE);
    expect_err(".", RP_E_DOT);
    expect_err("..", RP_E_DOT);
    expect_err("../evil", RP_E_DOT);
    expect_err("a/./b", RP_E_DOT);
    expect_err("a/../b", RP_E_DOT);
    expect_err("a/.", RP_E_DOT);
    expect_err("a//b", RP_E_EMPTY_COMP);
    expect_err("a/", RP_E_EMPTY_COMP);
    expect_err("a\\", RP_E_EMPTY_COMP);
    {
        char bad[] = { 'a', '/', (char)0xFF, '/', 'b', 0 };
        expect_err(bad, RP_E_INVALID_UTF8);
    }
    {
        char deep[64];
        int n = 0, i;
        for (i = 0; i < 17; i++) {
            if (i) deep[n++] = '/';
            deep[n++] = 'd';
        }
        deep[n] = 0;
        expect_err(deep, RP_E_DEPTH);
    }
    {
        char longrel[RP_REL_MAX + 2];
        memset(longrel, 'a', sizeof longrel - 1);
        longrel[sizeof longrel - 1] = 0;
        expect_err(longrel, RP_E_REL_TOO_LONG);
    }
}

static void test_sanitize(void)
{
    char out[RP_COMP_MAX + 1];
    bool trunc;

    trunc = true;
    CHECK(rp_sanitize_component("a:b*c?d\"e<f>g|h", out, sizeof out, &trunc));
    CHECK(strcmp(out, "a_b_c_d_e_f_g_h") == 0);
    CHECK(!trunc);

    CHECK(rp_sanitize_component("  a  ", out, sizeof out, &trunc));
    CHECK(strcmp(out, "a") == 0);

    CHECK(rp_sanitize_component("a. ", out, sizeof out, &trunc));
    CHECK(strcmp(out, "a") == 0);

    CHECK(rp_sanitize_component("???", out, sizeof out, &trunc));
    CHECK(strcmp(out, "unnamed") == 0);

    CHECK(rp_sanitize_component(".", out, sizeof out, &trunc));
    CHECK(strcmp(out, "unnamed") == 0);

    CHECK(rp_sanitize_component("CON", out, sizeof out, &trunc));
    CHECK(strcmp(out, "CON") == 0);

    {
        char big[256];
        memset(big, 'a', 200);
        big[200] = 0;
        CHECK(rp_sanitize_component(big, out, sizeof out, &trunc));
        CHECK(trunc);
        CHECK(strlen(out) == RP_COMP_MAX);
    }
    {
        char big[256];
        memset(big, 'a', 200);
        strcpy(big + 200, ".jpg");
        CHECK(rp_sanitize_component(big, out, sizeof out, &trunc));
        CHECK(trunc);
        CHECK(strlen(out) == RP_COMP_MAX);
        CHECK(strcmp(out + strlen(out) - 4, ".jpg") == 0);
    }
    {
        char mb[256];
        int n = 0, i;
        for (i = 0; i < 100; i++) {
            mb[n++] = (char)0xC3;
            mb[n++] = (char)0xA9;   /* é */
        }
        strcpy(mb + n, ".jpg");
        CHECK(rp_sanitize_component(mb, out, sizeof out, &trunc));
        CHECK(trunc);
        CHECK(strlen(out) <= RP_COMP_MAX);
        CHECK(rp_utf8_valid(out));
        CHECK(strcmp(out + strlen(out) - 4, ".jpg") == 0);
    }
    {
        char small[2];
        CHECK(rp_sanitize_component("abc", small, sizeof small, &trunc));
        CHECK(strcmp(small, "a") == 0);
        CHECK(trunc);
    }
    {
        char tiny[1];
        CHECK(!rp_sanitize_component("abc", tiny, sizeof tiny, &trunc));
        CHECK(tiny[0] == 0);
    }
    {
        char small[5];
        CHECK(!rp_sanitize_component("???", small, sizeof small, &trunc));
        CHECK(small[0] == 0);
    }
}

static void test_unique(void)
{
    char out[RP_COMP_MAX + 1];
    CHECK(rp_unique_name("a.jpg", 1, out, sizeof out));
    CHECK(strcmp(out, "a (1).jpg") == 0);
    CHECK(rp_unique_name("a", 2, out, sizeof out));
    CHECK(strcmp(out, "a (2)") == 0);
    CHECK(!rp_unique_name("a.jpg", 0, out, sizeof out));
    CHECK(out[0] == 0);
    {
        char small[4];
        CHECK(!rp_unique_name("a", 1, small, sizeof small));
        CHECK(small[0] == 0);
    }
    {
        char name[256];
        memset(name, 'b', RP_COMP_MAX);
        strcpy(name + RP_COMP_MAX, ".jpg");
        CHECK(rp_unique_name(name, 1, out, sizeof out));
        CHECK(strlen(out) <= RP_COMP_MAX);
        CHECK(strcmp(out + strlen(out) - 4, ".jpg") == 0);
        CHECK(strstr(out, " (1)") != NULL);
        CHECK(rp_utf8_valid(out));
    }
}

static void test_names(void)
{
    CHECK(rp_name_cmp("A.JPG", "a.jpg") == 0);
    CHECK(rp_name_cmp("Photos", "photos") == 0);
    CHECK(rp_name_cmp("A", "b") < 0);
    CHECK(rp_name_cmp("b", "A") > 0);
    CHECK(rp_name_eq("", ""));
    CHECK(!rp_name_eq("a", "aa"));

    CHECK(rp_reserved_name("Photos.psvsend-part"));
    CHECK(rp_reserved_name("x.PSVSEND.TMP"));
    CHECK(rp_reserved_name("x.psvsend.tmp"));
    CHECK(!rp_reserved_name("x.psvsend.tmp.txt"));
    CHECK(!rp_reserved_name("x.psvsend-part.txt"));
    CHECK(!rp_reserved_name("x"));
    CHECK(!rp_reserved_name(""));
}

static void test_paths(void)
{
    const char *dir = "ux0:data/PSVSend/Downloads";
    char out[1200];

    CHECK(rp_stage_root_path(dir, "Photos", out, sizeof out));
    CHECK(strcmp(out, "ux0:data/PSVSend/Downloads/Photos.psvsend-part") == 0);

    CHECK(rp_stage_file_path(dir, "Photos", "2026/a.jpg", out, sizeof out));
    CHECK(strcmp(out,
                 "ux0:data/PSVSend/Downloads/Photos.psvsend-part/2026/a.jpg") == 0);

    CHECK(rp_stage_part_path(dir, "Photos", "2026/a.jpg", out, sizeof out));
    CHECK(strcmp(out,
                 "ux0:data/PSVSend/Downloads/Photos.psvsend-part/2026/a.jpg"
                 ".psvsend.tmp") == 0);

    CHECK(rp_final_path(dir, "Photos", "2026/a.jpg", out, sizeof out));
    CHECK(strcmp(out, "ux0:data/PSVSend/Downloads/Photos/2026/a.jpg") == 0);

    CHECK(rp_final_path(dir, "a.jpg", NULL, out, sizeof out));
    CHECK(strcmp(out, "ux0:data/PSVSend/Downloads/a.jpg") == 0);

    CHECK(rp_flat_part_path(dir, "a.jpg", out, sizeof out));
    CHECK(strcmp(out, "ux0:data/PSVSend/Downloads/a.jpg.psvsend.tmp") == 0);

    CHECK(rp_stage_root_path("ux0:data/PSVSend/Downloads/", "Photos",
                             out, sizeof out));
    CHECK(strstr(out, "//") == NULL);

    CHECK(!rp_stage_file_path(dir, "Photos", "", out, sizeof out));
    CHECK(out[0] == 0);
    CHECK(!rp_stage_file_path(dir, "Photos", "../x", out, sizeof out));
    CHECK(out[0] == 0);
    CHECK(!rp_stage_root_path(dir, "a/b", out, sizeof out));
    CHECK(out[0] == 0);
    CHECK(!rp_stage_root_path(dir, "", out, sizeof out));
    CHECK(out[0] == 0);
    CHECK(!rp_stage_root_path(dir, "..", out, sizeof out));
    CHECK(out[0] == 0);

    {
        char longdir[1100];
        memset(longdir, 'x', 1000);
        longdir[1000] = 0;
        CHECK(rp_stage_root_path(longdir, "Photos", out, sizeof out));
        CHECK(strlen(out) == 1020);   /* 1000 + 1 + 6 + 13 */
        CHECK(!rp_stage_part_path(longdir, "Photos", "0123456789",
                                  out, sizeof out));
        CHECK(out[0] == 0);
        CHECK(rp_final_path(longdir, "Photos", "0123456789", out, sizeof out));
    }
    {
        char longdir[1100];
        memset(longdir, 'x', 1020);
        longdir[1020] = 0;
        CHECK(!rp_stage_root_path(longdir, "Photos", out, sizeof out));
        CHECK(out[0] == 0);
    }
    {
        char small[8];
        CHECK(!rp_flat_part_path(dir, "a.jpg", small, sizeof small));
        CHECK(small[0] == 0);
    }
}

static void test_buffer(void)
{
    char out[4];
    int depth = -1;
    bool trunc = false;
    CHECK(rp_parse_relpath("abcdef", out, sizeof out, &depth, &trunc) ==
          RP_E_BUFFER);
    CHECK(out[0] == 0);
    CHECK(rp_parse_relpath("a", out, 1, &depth, &trunc) == RP_E_BUFFER);
    CHECK(out[0] == 0);
    CHECK(rp_parse_relpath("a", out, 0, &depth, &trunc) == RP_E_BUFFER);
}

int main(void)
{
    test_utf8();
    test_parse_ok();
    test_parse_err();
    test_sanitize();
    test_unique();
    test_names();
    test_paths();
    test_buffer();

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all recv_path checks passed\n");
    return 0;
}
