#include "unity.h"
#include <stdio.h>
#include <string.h>

/* Unity Internal State */
static const char* current_test_name;
static int unity_test_count = 0;
static int unity_test_failures = 0;
static int unity_test_ignores = 0;

int unity_result = 0;

/* Begin Unity Test Suite */
void UnityBegin(const char* filename) {
    unity_test_count = 0;
    unity_test_failures = 0;
    unity_test_ignores = 0;
    
    printf("\n");
    printf("==========================================\n");
    printf("Unity Test Suite: %s\n", filename);
    printf("==========================================\n");
}

/* End Unity Test Suite */
int UnityEnd(void) {
    printf("\n");
    printf("-----------------------------------------\n");
    printf("%d Tests %d Failures %d Ignored\n", 
           unity_test_count, unity_test_failures, unity_test_ignores);
    
    if (unity_test_failures == 0) {
        printf("OK\n");
        unity_result = 0;
    } else {
        printf("FAIL\n");
        unity_result = 1;
    }
    printf("==========================================\n");
    
    return unity_result;
}

/* Run a single test */
void UnityDefaultTestRun(void (*Func)(void), const char* FuncName, const int FuncLineNum) {
    (void)FuncLineNum; // Suppress unused parameter warning
    
    current_test_name = FuncName;
    unity_test_count++;
    
    printf("Running %s... ", FuncName);
    fflush(stdout);
    
    /* Run the test directly */
    Func();
    
    printf("PASS\n");
}

/* Conclude test (called after assertion failure) */
void UnityConcludeTest(void) {
    unity_test_failures++;
}

/* Unity Print Functions */
void UnityPrint(const char* string) {
    printf("%s", string);
}

void UnityPrintNumber(const long number) {
    printf("%ld", number);
}

/* Unity Fail */
void UnityFail(const char* message, const int line) {
    printf("FAIL\n");
    printf("  %s:%d: %s\n", current_test_name, line, message);
    UnityConcludeTest();
}

/* Assertions */
void UNITY_TEST_ASSERT(const int condition, const unsigned short line, const char* message) {
    if (!condition) {
        UnityFail(message, line);
    }
}

void UNITY_TEST_ASSERT_NULL(const void* pointer, const unsigned short line, const char* message) {
    if (pointer != NULL) {
        UnityFail(message, line);
    }
}

void UNITY_TEST_ASSERT_NOT_NULL(const void* pointer, const unsigned short line, const char* message) {
    if (pointer == NULL) {
        UnityFail(message, line);
    }
}

void UNITY_TEST_ASSERT_EQUAL_INT(const int expected, const int actual, const unsigned short line, const char* message) {
    if (expected != actual) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Expected %d Was %d. %s", 
                 expected, actual, message ? message : "");
        UnityFail(buffer, line);
    }
}

void UNITY_TEST_ASSERT_EQUAL_UINT(const unsigned int expected, const unsigned int actual, const unsigned short line, const char* message) {
    if (expected != actual) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Expected %u Was %u. %s", 
                 expected, actual, message ? message : "");
        UnityFail(buffer, line);
    }
}

void UNITY_TEST_ASSERT_EQUAL_STRING(const char* expected, const char* actual, const unsigned short line, const char* message) {
    if (expected == NULL && actual == NULL) {
        return; /* Both NULL - equal */
    }
    
    if (expected == NULL || actual == NULL) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Expected '%s' Was '%s'. %s",
                 expected ? expected : "NULL",
                 actual ? actual : "NULL",
                 message ? message : "");
        UnityFail(buffer, line);
        return;
    }
    
    if (strcmp(expected, actual) != 0) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "Expected '%s' Was '%s'. %s",
                 expected, actual, message ? message : "");
        UnityFail(buffer, line);
    }
}

void UNITY_TEST_ASSERT_GREATER_OR_EQUAL_INT(const int threshold, const int actual, const unsigned short line, const char* message) {
    if (actual < threshold) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Expected %d to be >= %d. %s",
                 actual, threshold, message ? message : "");
        UnityFail(buffer, line);
    }
}

void UNITY_TEST_ASSERT_GREATER_THAN_INT(const int threshold, const int actual, const unsigned short line, const char* message) {
    if (actual <= threshold) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Expected %d to be > %d. %s",
                 actual, threshold, message ? message : "");
        UnityFail(buffer, line);
    }
}
