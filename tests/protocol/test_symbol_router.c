#include "unity.h"
#include "protocol/symbol_router.h"
#include <string.h>

/* ============================================================================
 * Symbol Router Unit Tests
 *
 * Tests the symbol-based processor routing logic:
 *   - Symbols A-M → Processor 0
 *   - Symbols N-Z → Processor 1
 *   - Edge cases: boundaries, lowercase, non-alpha, null/empty
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * Basic Routing Tests
 * ---------------------------------------------------------------------------- */

/* Test: A-M symbols route to Processor 0 */
void test_SymbolRouter_AtoM_RoutesToProcessor0(void) {
    /* First letters A through M should all go to processor 0 */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("AAPL"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("BAC"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("CAT"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("DIS"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("EBAY"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("F"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("GOOGL"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("HD"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("IBM"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("JPM"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("KO"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("LMT"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("META"));
}

/* Test: N-Z symbols route to Processor 1 */
void test_SymbolRouter_NtoZ_RoutesToProcessor1(void) {
    /* First letters N through Z should all go to processor 1 */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("NVDA"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("ORCL"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("PG"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("QCOM"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("RTX"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("SPY"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("TSLA"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("UBER"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("V"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("WMT"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("XOM"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("YUM"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("ZM"));
}

/* ----------------------------------------------------------------------------
 * Boundary Tests (M/N boundary is critical)
 * ---------------------------------------------------------------------------- */

/* Test: M is the last letter routing to Processor 0 */
void test_SymbolRouter_M_BoundaryProcessor0(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("M"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("MSFT"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("MCD"));
}

/* Test: N is the first letter routing to Processor 1 */
void test_SymbolRouter_N_BoundaryProcessor1(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("N"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("NFLX"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("NKE"));
}

/* Test: A is the first letter (start of range) */
void test_SymbolRouter_A_StartBoundary(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("A"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("AMZN"));
}

/* Test: Z is the last letter (end of range) */
void test_SymbolRouter_Z_EndBoundary(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("Z"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("ZNGA"));
}

/* ----------------------------------------------------------------------------
 * Lowercase Normalization Tests
 * ---------------------------------------------------------------------------- */

/* Test: Lowercase a-m symbols route to Processor 0 */
void test_SymbolRouter_Lowercase_AtoM(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("aapl"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("ibm"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("meta"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("m"));
}

/* Test: Lowercase n-z symbols route to Processor 1 */
void test_SymbolRouter_Lowercase_NtoZ(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("nvda"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("tsla"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("zm"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("n"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("z"));
}

/* Test: Mixed case symbols (only first char matters) */
void test_SymbolRouter_MixedCase(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("iBm"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("tSlA"));
}

/* ----------------------------------------------------------------------------
 * Edge Cases and Invalid Input
 * ---------------------------------------------------------------------------- */

/* Test: NULL symbol defaults to Processor 0 */
void test_SymbolRouter_NullSymbol(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol(NULL));
}

/* Test: Empty string defaults to Processor 0 */
void test_SymbolRouter_EmptySymbol(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol(""));
}

/* Test: Numeric symbols default to Processor 0 */
void test_SymbolRouter_NumericSymbol(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("1234"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("0"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("9XYZ"));
}

/* Test: Special character symbols default to Processor 0 */
void test_SymbolRouter_SpecialCharSymbol(void) {
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("$SPX"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol(".DJI"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("^GSPC"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("-TEST"));
}

/* Test: Single character symbols */
void test_SymbolRouter_SingleCharSymbols(void) {
    /* A-M single chars */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("A"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("F"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("M"));

    /* N-Z single chars */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("N"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("T"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("Z"));
}

/* ----------------------------------------------------------------------------
 * Validation Function Tests
 * ---------------------------------------------------------------------------- */

/* Test: symbol_is_valid with valid symbols (must start with a letter) */
void test_SymbolIsValid_ValidSymbols(void) {
    TEST_ASSERT_TRUE(symbol_is_valid("IBM"));
    TEST_ASSERT_TRUE(symbol_is_valid("A"));
    TEST_ASSERT_TRUE(symbol_is_valid("AAPL"));
    TEST_ASSERT_TRUE(symbol_is_valid("TSLA"));
    TEST_ASSERT_TRUE(symbol_is_valid("a"));      /* Lowercase OK */
    TEST_ASSERT_TRUE(symbol_is_valid("test"));   /* Lowercase OK */
}

/* Test: symbol_is_valid with invalid symbols */
void test_SymbolIsValid_InvalidSymbols(void) {
    TEST_ASSERT_FALSE(symbol_is_valid(NULL));
    TEST_ASSERT_FALSE(symbol_is_valid(""));
    /* Numeric-first symbols are invalid (handled by router but not "valid") */
    TEST_ASSERT_FALSE(symbol_is_valid("1234"));
    TEST_ASSERT_FALSE(symbol_is_valid("$SPX"));
}

/* ----------------------------------------------------------------------------
 * Processor Name Tests
 * ---------------------------------------------------------------------------- */

/* Test: get_processor_name returns correct names */
void test_GetProcessorName(void) {
    TEST_ASSERT_EQUAL_STRING("A-M", get_processor_name(PROCESSOR_ID_A_TO_M));
    TEST_ASSERT_EQUAL_STRING("N-Z", get_processor_name(PROCESSOR_ID_N_TO_Z));
    TEST_ASSERT_EQUAL_STRING("Unknown", get_processor_name(-1));
    TEST_ASSERT_EQUAL_STRING("Unknown", get_processor_name(99));
}

/* ----------------------------------------------------------------------------
 * Consistency Tests
 * ---------------------------------------------------------------------------- */

/* Test: Same symbol always routes to same processor */
void test_SymbolRouter_Consistency(void) {
    /* Call multiple times to ensure deterministic behavior */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("IBM"));
        TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("TSLA"));
    }
}

/* Test: Processor ID values are valid */
void test_SymbolRouter_ValidProcessorIds(void) {
    int id_a = get_processor_id_for_symbol("AAPL");
    int id_n = get_processor_id_for_symbol("NVDA");

    TEST_ASSERT_TRUE(id_a >= 0 && id_a < NUM_PROCESSORS);
    TEST_ASSERT_TRUE(id_n >= 0 && id_n < NUM_PROCESSORS);
    TEST_ASSERT_TRUE(id_a != id_n);
}
