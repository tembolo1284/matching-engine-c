#ifndef UNITY_FRAMEWORK_H
#define UNITY_FRAMEWORK_H

#include <setjmp.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>

/* Unity Configuration */
#define UNITY_OUTPUT_CHAR(a)    putchar(a)
#define UNITY_OUTPUT_FLUSH()    fflush(stdout)
#define UNITY_OUTPUT_START()    
#define UNITY_OUTPUT_COMPLETE()

/* Return values */
#define UNITY_RETURN_CODE   unity_result

/* Test Assertions */
#define TEST_ASSERT_TRUE(condition)                   UNITY_TEST_ASSERT(     (condition), __LINE__, " Expected TRUE")
#define TEST_ASSERT_FALSE(condition)                  UNITY_TEST_ASSERT(    !(condition), __LINE__, " Expected FALSE")
#define TEST_ASSERT_NULL(pointer)                     UNITY_TEST_ASSERT_NULL(    (pointer), __LINE__, " Expected NULL")
#define TEST_ASSERT_NOT_NULL(pointer)                 UNITY_TEST_ASSERT_NOT_NULL((pointer), __LINE__, " Expected Non-NULL")

#define TEST_ASSERT_EQUAL(expected, actual)           UNITY_TEST_ASSERT_EQUAL_INT((expected), (actual), __LINE__, NULL)
#define TEST_ASSERT_EQUAL_INT(expected, actual)       UNITY_TEST_ASSERT_EQUAL_INT((expected), (actual), __LINE__, NULL)
#define TEST_ASSERT_EQUAL_UINT(expected, actual)      UNITY_TEST_ASSERT_EQUAL_UINT((expected), (actual), __LINE__, NULL)
#define TEST_ASSERT_EQUAL_STRING(expected, actual)    UNITY_TEST_ASSERT_EQUAL_STRING((expected), (actual), __LINE__, NULL)

#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual)  UNITY_TEST_ASSERT_GREATER_OR_EQUAL_INT((threshold), (actual), __LINE__, NULL)
#define TEST_ASSERT_GREATER_THAN(threshold, actual)      UNITY_TEST_ASSERT_GREATER_THAN_INT((threshold), (actual), __LINE__, NULL)

/* Unity Macros */
#define UNITY_BEGIN()    UnityBegin(__FILE__)
#define UNITY_END()      UnityEnd()
#define RUN_TEST(func)   UnityDefaultTestRun(func, #func, __LINE__)

/* Unity Functions */
void UnityBegin(const char* filename);
int  UnityEnd(void);
void UnityConcludeTest(void);
void UnityDefaultTestRun(void (*Func)(void), const char* FuncName, const int FuncLineNum);

void UnityFail(const char* message, const int line);
void UnityPrint(const char* string);
void UnityPrintNumber(const long number);

void UNITY_TEST_ASSERT(const int condition, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_NULL(const void* pointer, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_NOT_NULL(const void* pointer, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_EQUAL_INT(const int expected, const int actual, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_EQUAL_UINT(const unsigned int expected, const unsigned int actual, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_EQUAL_STRING(const char* expected, const char* actual, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_GREATER_OR_EQUAL_INT(const int threshold, const int actual, const unsigned short line, const char* message);
void UNITY_TEST_ASSERT_GREATER_THAN_INT(const int threshold, const int actual, const unsigned short line, const char* message);

/* Test result */
extern int unity_result;

/* Setup/Teardown */
extern void (*setUp)(void);
extern void (*tearDown)(void);

#endif /* UNITY_FRAMEWORK_H */
