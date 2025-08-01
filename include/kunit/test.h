/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Base unit test (KUnit) API.
 *
 * Copyright (C) 2019, Google LLC.
 * Author: Brendan Higgins <brendanhiggins@google.com>
 */

#ifndef _KUNIT_TEST_H
#define _KUNIT_TEST_H

#include <kunit/assert.h>
#include <kunit/try-catch.h>

#include <linux/args.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kconfig.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/rwonce.h>
#include <asm/sections.h>

/* Static key: true if any KUnit tests are currently running */
DECLARE_STATIC_KEY_FALSE(kunit_running);

struct kunit;
struct string_stream;

/* Maximum size of parameter description string. */
#define KUNIT_PARAM_DESC_SIZE 128

/* Maximum size of a status comment. */
#define KUNIT_STATUS_COMMENT_SIZE 256

/*
 * TAP specifies subtest stream indentation of 4 spaces, 8 spaces for a
 * sub-subtest.  See the "Subtests" section in
 * https://node-tap.org/tap-protocol/
 */
#define KUNIT_INDENT_LEN		4
#define KUNIT_SUBTEST_INDENT		"    "
#define KUNIT_SUBSUBTEST_INDENT		"        "

/**
 * enum kunit_status - Type of result for a test or test suite
 * @KUNIT_SUCCESS: Denotes the test suite has not failed nor been skipped
 * @KUNIT_FAILURE: Denotes the test has failed.
 * @KUNIT_SKIPPED: Denotes the test has been skipped.
 */
enum kunit_status {
	KUNIT_SUCCESS,
	KUNIT_FAILURE,
	KUNIT_SKIPPED,
};

/* Attribute struct/enum definitions */

/*
 * Speed Attribute is stored as an enum and separated into categories of
 * speed: very_slow, slow, and normal. These speeds are relative to
 * other KUnit tests.
 *
 * Note: unset speed attribute acts as default of KUNIT_SPEED_NORMAL.
 */
enum kunit_speed {
	KUNIT_SPEED_UNSET,
	KUNIT_SPEED_VERY_SLOW,
	KUNIT_SPEED_SLOW,
	KUNIT_SPEED_NORMAL,
	KUNIT_SPEED_MAX = KUNIT_SPEED_NORMAL,
};

/* Holds attributes for each test case and suite */
struct kunit_attributes {
	enum kunit_speed speed;
};

/**
 * struct kunit_case - represents an individual test case.
 *
 * @run_case: the function representing the actual test case.
 * @name:     the name of the test case.
 * @generate_params: the generator function for parameterized tests.
 * @attr:     the attributes associated with the test
 *
 * A test case is a function with the signature,
 * ``void (*)(struct kunit *)``
 * that makes expectations and assertions (see KUNIT_EXPECT_TRUE() and
 * KUNIT_ASSERT_TRUE()) about code under test. Each test case is associated
 * with a &struct kunit_suite and will be run after the suite's init
 * function and followed by the suite's exit function.
 *
 * A test case should be static and should only be created with the
 * KUNIT_CASE() macro; additionally, every array of test cases should be
 * terminated with an empty test case.
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	void add_test_basic(struct kunit *test)
 *	{
 *		KUNIT_EXPECT_EQ(test, 1, add(1, 0));
 *		KUNIT_EXPECT_EQ(test, 2, add(1, 1));
 *		KUNIT_EXPECT_EQ(test, 0, add(-1, 1));
 *		KUNIT_EXPECT_EQ(test, INT_MAX, add(0, INT_MAX));
 *		KUNIT_EXPECT_EQ(test, -1, add(INT_MAX, INT_MIN));
 *	}
 *
 *	static struct kunit_case example_test_cases[] = {
 *		KUNIT_CASE(add_test_basic),
 *		{}
 *	};
 *
 */
struct kunit_case {
	void (*run_case)(struct kunit *test);
	const char *name;
	const void* (*generate_params)(const void *prev, char *desc);
	struct kunit_attributes attr;

	/* private: internal use only. */
	enum kunit_status status;
	char *module_name;
	struct string_stream *log;
};

static inline char *kunit_status_to_ok_not_ok(enum kunit_status status)
{
	switch (status) {
	case KUNIT_SKIPPED:
	case KUNIT_SUCCESS:
		return "ok";
	case KUNIT_FAILURE:
		return "not ok";
	}
	return "invalid";
}

/**
 * KUNIT_CASE - A helper for creating a &struct kunit_case
 *
 * @test_name: a reference to a test case function.
 *
 * Takes a symbol for a function representing a test case and creates a
 * &struct kunit_case object from it. See the documentation for
 * &struct kunit_case for an example on how to use it.
 */
#define KUNIT_CASE(test_name)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_ATTR - A helper for creating a &struct kunit_case
 * with attributes
 *
 * @test_name: a reference to a test case function.
 * @attributes: a reference to a struct kunit_attributes object containing
 * test attributes
 */
#define KUNIT_CASE_ATTR(test_name, attributes)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .attr = attributes, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_SLOW - A helper for creating a &struct kunit_case
 * with the slow attribute
 *
 * @test_name: a reference to a test case function.
 */

#define KUNIT_CASE_SLOW(test_name)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .attr.speed = KUNIT_SPEED_SLOW, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_PARAM - A helper for creation a parameterized &struct kunit_case
 *
 * @test_name: a reference to a test case function.
 * @gen_params: a reference to a parameter generator function.
 *
 * The generator function::
 *
 *	const void* gen_params(const void *prev, char *desc)
 *
 * is used to lazily generate a series of arbitrarily typed values that fit into
 * a void*. The argument @prev is the previously returned value, which should be
 * used to derive the next value; @prev is set to NULL on the initial generator
 * call. When no more values are available, the generator must return NULL.
 * Optionally write a string into @desc (size of KUNIT_PARAM_DESC_SIZE)
 * describing the parameter.
 */
#define KUNIT_CASE_PARAM(test_name, gen_params)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .generate_params = gen_params, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_PARAM_ATTR - A helper for creating a parameterized &struct
 * kunit_case with attributes
 *
 * @test_name: a reference to a test case function.
 * @gen_params: a reference to a parameter generator function.
 * @attributes: a reference to a struct kunit_attributes object containing
 * test attributes
 */
#define KUNIT_CASE_PARAM_ATTR(test_name, gen_params, attributes)	\
		{ .run_case = test_name, .name = #test_name,	\
		  .generate_params = gen_params,				\
		  .attr = attributes, .module_name = KBUILD_MODNAME}

/**
 * struct kunit_suite - describes a related collection of &struct kunit_case
 *
 * @name:	the name of the test. Purely informational.
 * @suite_init:	called once per test suite before the test cases.
 * @suite_exit:	called once per test suite after all test cases.
 * @init:	called before every test case.
 * @exit:	called after every test case.
 * @test_cases:	a null terminated array of test cases.
 * @attr:	the attributes associated with the test suite
 *
 * A kunit_suite is a collection of related &struct kunit_case s, such that
 * @init is called before every test case and @exit is called after every
 * test case, similar to the notion of a *test fixture* or a *test class*
 * in other unit testing frameworks like JUnit or Googletest.
 *
 * Note that @exit and @suite_exit will run even if @init or @suite_init
 * fail: make sure they can handle any inconsistent state which may result.
 *
 * Every &struct kunit_case must be associated with a kunit_suite for KUnit
 * to run it.
 */
struct kunit_suite {
	const char name[256];
	int (*suite_init)(struct kunit_suite *suite);
	void (*suite_exit)(struct kunit_suite *suite);
	int (*init)(struct kunit *test);
	void (*exit)(struct kunit *test);
	struct kunit_case *test_cases;
	struct kunit_attributes attr;

	/* private: internal use only */
	char status_comment[KUNIT_STATUS_COMMENT_SIZE];
	struct dentry *debugfs;
	struct string_stream *log;
	int suite_init_err;
	bool is_init;
};

/* Stores an array of suites, end points one past the end */
struct kunit_suite_set {
	struct kunit_suite * const *start;
	struct kunit_suite * const *end;
};

/**
 * struct kunit - represents a running instance of a test.
 *
 * @priv: for user to store arbitrary data. Commonly used to pass data
 *	  created in the init function (see &struct kunit_suite).
 *
 * Used to store information about the current context under which the test
 * is running. Most of this data is private and should only be accessed
 * indirectly via public functions; the one exception is @priv which can be
 * used by the test writer to store arbitrary data.
 */
struct kunit {
	void *priv;

	/* private: internal use only. */
	const char *name; /* Read only after initialization! */
	struct string_stream *log; /* Points at case log after initialization */
	struct kunit_try_catch try_catch;
	/* param_value is the current parameter value for a test case. */
	const void *param_value;
	/* param_index stores the index of the parameter in parameterized tests. */
	int param_index;
	/*
	 * success starts as true, and may only be set to false during a
	 * test case; thus, it is safe to update this across multiple
	 * threads using WRITE_ONCE; however, as a consequence, it may only
	 * be read after the test case finishes once all threads associated
	 * with the test case have terminated.
	 */
	spinlock_t lock; /* Guards all mutable test state. */
	enum kunit_status status; /* Read only after test_case finishes! */
	/*
	 * Because resources is a list that may be updated multiple times (with
	 * new resources) from any thread associated with a test case, we must
	 * protect it with some type of lock.
	 */
	struct list_head resources; /* Protected by lock. */

	char status_comment[KUNIT_STATUS_COMMENT_SIZE];
	/* Saves the last seen test. Useful to help with faults. */
	struct kunit_loc last_seen;
};

static inline void kunit_set_failure(struct kunit *test)
{
	WRITE_ONCE(test->status, KUNIT_FAILURE);
}

bool kunit_enabled(void);
bool kunit_autorun(void);
const char *kunit_action(void);
const char *kunit_filter_glob(void);
char *kunit_filter(void);
char *kunit_filter_action(void);

void kunit_init_test(struct kunit *test, const char *name, struct string_stream *log);

int kunit_run_tests(struct kunit_suite *suite);

size_t kunit_suite_num_test_cases(struct kunit_suite *suite);

unsigned int kunit_test_case_num(struct kunit_suite *suite,
				 struct kunit_case *test_case);

struct kunit_suite_set
kunit_filter_suites(const struct kunit_suite_set *suite_set,
		    const char *filter_glob,
		    char *filters,
		    char *filter_action,
		    int *err);
void kunit_free_suite_set(struct kunit_suite_set suite_set);

int __kunit_test_suites_init(struct kunit_suite * const * const suites, int num_suites,
			     bool run_tests);

void __kunit_test_suites_exit(struct kunit_suite **suites, int num_suites);

void kunit_exec_run_tests(struct kunit_suite_set *suite_set, bool builtin);
void kunit_exec_list_tests(struct kunit_suite_set *suite_set, bool include_attr);

struct kunit_suite_set kunit_merge_suite_sets(struct kunit_suite_set init_suite_set,
		struct kunit_suite_set suite_set);

#if IS_BUILTIN(CONFIG_KUNIT)
int kunit_run_all_tests(void);
#else
static inline int kunit_run_all_tests(void)
{
	return 0;
}
#endif /* IS_BUILTIN(CONFIG_KUNIT) */

#define __kunit_test_suites(unique_array, ...)				       \
	static struct kunit_suite *unique_array[]			       \
	__aligned(sizeof(struct kunit_suite *))				       \
	__used __section(".kunit_test_suites") = { __VA_ARGS__ }

/**
 * kunit_test_suites() - used to register one or more &struct kunit_suite
 *			 with KUnit.
 *
 * @__suites: a statically allocated list of &struct kunit_suite.
 *
 * Registers @suites with the test framework.
 * This is done by placing the array of struct kunit_suite * in the
 * .kunit_test_suites ELF section.
 *
 * When builtin, KUnit tests are all run via the executor at boot, and when
 * built as a module, they run on module load.
 *
 */
#define kunit_test_suites(__suites...)						\
	__kunit_test_suites(__UNIQUE_ID(array),				\
			    ##__suites)

#define kunit_test_suite(suite)	kunit_test_suites(&suite)

#define __kunit_init_test_suites(unique_array, ...)			       \
	static struct kunit_suite *unique_array[]			       \
	__aligned(sizeof(struct kunit_suite *))				       \
	__used __section(".kunit_init_test_suites") = { __VA_ARGS__ }

/**
 * kunit_test_init_section_suites() - used to register one or more &struct
 *				      kunit_suite containing init functions or
 *				      init data.
 *
 * @__suites: a statically allocated list of &struct kunit_suite.
 *
 * This functions similar to kunit_test_suites() except that it compiles the
 * list of suites during init phase.
 *
 * This macro also suffixes the array and suite declarations it makes with
 * _probe; so that modpost suppresses warnings about referencing init data
 * for symbols named in this manner.
 *
 * Note: these init tests are not able to be run after boot so there is no
 * "run" debugfs file generated for these tests.
 *
 * Also, do not mark the suite or test case structs with __initdata because
 * they will be used after the init phase with debugfs.
 */
#define kunit_test_init_section_suites(__suites...)			\
	__kunit_init_test_suites(CONCATENATE(__UNIQUE_ID(array), _probe), \
			    ##__suites)

#define kunit_test_init_section_suite(suite)	\
	kunit_test_init_section_suites(&suite)

#define kunit_suite_for_each_test_case(suite, test_case)		\
	for (test_case = suite->test_cases; test_case->run_case; test_case++)

enum kunit_status kunit_suite_has_succeeded(struct kunit_suite *suite);

/**
 * kunit_kmalloc_array() - Like kmalloc_array() except the allocation is *test managed*.
 * @test: The test context object.
 * @n: number of elements.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * Just like `kmalloc_array(...)`, except the allocation is managed by the test case
 * and is automatically cleaned up after the test case concludes. See kunit_add_action()
 * for more information.
 *
 * Note that some internal context data is also allocated with GFP_KERNEL,
 * regardless of the gfp passed in.
 */
void *kunit_kmalloc_array(struct kunit *test, size_t n, size_t size, gfp_t gfp);

/**
 * kunit_kmalloc() - Like kmalloc() except the allocation is *test managed*.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kmalloc() and kunit_kmalloc_array() for more information.
 *
 * Note that some internal context data is also allocated with GFP_KERNEL,
 * regardless of the gfp passed in.
 */
static inline void *kunit_kmalloc(struct kunit *test, size_t size, gfp_t gfp)
{
	return kunit_kmalloc_array(test, 1, size, gfp);
}

/**
 * kunit_kfree() - Like kfree except for allocations managed by KUnit.
 * @test: The test case to which the resource belongs.
 * @ptr: The memory allocation to free.
 */
void kunit_kfree(struct kunit *test, const void *ptr);

/**
 * kunit_kzalloc() - Just like kunit_kmalloc(), but zeroes the allocation.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kzalloc() and kunit_kmalloc_array() for more information.
 */
static inline void *kunit_kzalloc(struct kunit *test, size_t size, gfp_t gfp)
{
	return kunit_kmalloc(test, size, gfp | __GFP_ZERO);
}

/**
 * kunit_kcalloc() - Just like kunit_kmalloc_array(), but zeroes the allocation.
 * @test: The test context object.
 * @n: number of elements.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kcalloc() and kunit_kmalloc_array() for more information.
 */
static inline void *kunit_kcalloc(struct kunit *test, size_t n, size_t size, gfp_t gfp)
{
	return kunit_kmalloc_array(test, n, size, gfp | __GFP_ZERO);
}


/**
 * kunit_kfree_const() - conditionally free test managed memory
 * @test: The test context object.
 * @x: pointer to the memory
 *
 * Calls kunit_kfree() only if @x is not in .rodata section.
 * See kunit_kstrdup_const() for more information.
 */
void kunit_kfree_const(struct kunit *test, const void *x);

/**
 * kunit_kstrdup() - Duplicates a string into a test managed allocation.
 *
 * @test: The test context object.
 * @str: The NULL-terminated string to duplicate.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kstrdup() and kunit_kmalloc_array() for more information.
 */
static inline char *kunit_kstrdup(struct kunit *test, const char *str, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!str)
		return NULL;

	len = strlen(str) + 1;
	buf = kunit_kmalloc(test, len, gfp);
	if (buf)
		memcpy(buf, str, len);
	return buf;
}

/**
 * kunit_kstrdup_const() - Conditionally duplicates a string into a test managed allocation.
 *
 * @test: The test context object.
 * @str: The NULL-terminated string to duplicate.
 * @gfp: flags passed to underlying kmalloc().
 *
 * Calls kunit_kstrdup() only if @str is not in the rodata section. Must be freed with
 * kunit_kfree_const() -- not kunit_kfree().
 * See kstrdup_const() and kunit_kmalloc_array() for more information.
 */
const char *kunit_kstrdup_const(struct kunit *test, const char *str, gfp_t gfp);

/**
 * kunit_attach_mm() - Create and attach a new mm if it doesn't already exist.
 *
 * Allocates a &struct mm_struct and attaches it to @current. In most cases, call
 * kunit_vm_mmap() without calling kunit_attach_mm() directly. Only necessary when
 * code under test accesses the mm before executing the mmap (e.g., to perform
 * additional initialization beforehand).
 *
 * Return: 0 on success, -errno on failure.
 */
int kunit_attach_mm(void);

/**
 * kunit_vm_mmap() - Allocate KUnit-tracked vm_mmap() area
 * @test: The test context object.
 * @file: struct file pointer to map from, if any
 * @addr: desired address, if any
 * @len: how many bytes to allocate
 * @prot: mmap PROT_* bits
 * @flag: mmap flags
 * @offset: offset into @file to start mapping from.
 *
 * See vm_mmap() for more information.
 */
unsigned long kunit_vm_mmap(struct kunit *test, struct file *file,
			    unsigned long addr, unsigned long len,
			    unsigned long prot, unsigned long flag,
			    unsigned long offset);

void kunit_cleanup(struct kunit *test);

void __printf(2, 3) kunit_log_append(struct string_stream *log, const char *fmt, ...);

/**
 * kunit_mark_skipped() - Marks @test as skipped
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Marks the test as skipped. @fmt is given output as the test status
 * comment, typically the reason the test was skipped.
 *
 * Test execution continues after kunit_mark_skipped() is called.
 */
#define kunit_mark_skipped(test, fmt, ...)				\
	do {								\
		WRITE_ONCE((test)->status, KUNIT_SKIPPED);		\
		scnprintf((test)->status_comment,			\
			  KUNIT_STATUS_COMMENT_SIZE,			\
			  fmt, ##__VA_ARGS__);				\
	} while (0)

/**
 * kunit_skip() - Marks @test as skipped
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Skips the test. @fmt is given output as the test status
 * comment, typically the reason the test was skipped.
 *
 * Test execution is halted after kunit_skip() is called.
 */
#define kunit_skip(test, fmt, ...)					\
	do {								\
		kunit_mark_skipped((test), fmt, ##__VA_ARGS__);		\
		kunit_try_catch_throw(&((test)->try_catch));		\
	} while (0)

/*
 * printk and log to per-test or per-suite log buffer.  Logging only done
 * if CONFIG_KUNIT_DEBUGFS is 'y'; if it is 'n', no log is allocated/used.
 */
#define kunit_log(lvl, test_or_suite, fmt, ...)				\
	do {								\
		printk(lvl fmt, ##__VA_ARGS__);				\
		kunit_log_append((test_or_suite)->log,	fmt,		\
				 ##__VA_ARGS__);			\
	} while (0)

#define kunit_printk(lvl, test, fmt, ...)				\
	kunit_log(lvl, test, KUNIT_SUBTEST_INDENT "# %s: " fmt,		\
		  (test)->name,	##__VA_ARGS__)

/**
 * kunit_info() - Prints an INFO level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints an info level message associated with the test suite being run.
 * Takes a variable number of format parameters just like printk().
 */
#define kunit_info(test, fmt, ...) \
	kunit_printk(KERN_INFO, test, fmt, ##__VA_ARGS__)

/**
 * kunit_warn() - Prints a WARN level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints a warning level message.
 */
#define kunit_warn(test, fmt, ...) \
	kunit_printk(KERN_WARNING, test, fmt, ##__VA_ARGS__)

/**
 * kunit_err() - Prints an ERROR level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints an error level message.
 */
#define kunit_err(test, fmt, ...) \
	kunit_printk(KERN_ERR, test, fmt, ##__VA_ARGS__)

/*
 * Must be called at the beginning of each KUNIT_*_ASSERTION().
 * Cf. KUNIT_CURRENT_LOC.
 */
#define _KUNIT_SAVE_LOC(test) do {					       \
	WRITE_ONCE(test->last_seen.file, __FILE__);			       \
	WRITE_ONCE(test->last_seen.line, __LINE__);			       \
} while (0)

/**
 * KUNIT_SUCCEED() - A no-op expectation. Only exists for code clarity.
 * @test: The test context object.
 *
 * The opposite of KUNIT_FAIL(), it is an expectation that cannot fail. In other
 * words, it does nothing and only exists for code clarity. See
 * KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_SUCCEED(test) _KUNIT_SAVE_LOC(test)

void __noreturn __kunit_abort(struct kunit *test);

void __printf(6, 7) __kunit_do_failed_assertion(struct kunit *test,
						const struct kunit_loc *loc,
						enum kunit_assert_type type,
						const struct kunit_assert *assert,
						assert_format_t assert_format,
						const char *fmt, ...);

#define _KUNIT_FAILED(test, assert_type, assert_class, assert_format, INITIALIZER, fmt, ...) do { \
	static const struct kunit_loc __loc = KUNIT_CURRENT_LOC;	       \
	const struct assert_class __assertion = INITIALIZER;		       \
	__kunit_do_failed_assertion(test,				       \
				    &__loc,				       \
				    assert_type,			       \
				    &__assertion.assert,		       \
				    assert_format,			       \
				    fmt,				       \
				    ##__VA_ARGS__);			       \
	if (assert_type == KUNIT_ASSERTION)				       \
		__kunit_abort(test);					       \
} while (0)


#define KUNIT_FAIL_ASSERTION(test, assert_type, fmt, ...) do {		       \
	_KUNIT_SAVE_LOC(test);						       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_fail_assert,				       \
		      kunit_fail_assert_format,				       \
		      {},						       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

/**
 * KUNIT_FAIL() - Always causes a test to fail when evaluated.
 * @test: The test context object.
 * @fmt: an informational message to be printed when the assertion is made.
 * @...: string format arguments.
 *
 * The opposite of KUNIT_SUCCEED(), it is an expectation that always fails. In
 * other words, it always results in a failed expectation, and consequently
 * always causes the test case to fail when evaluated. See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_FAIL(test, fmt, ...)					       \
	KUNIT_FAIL_ASSERTION(test,					       \
			     KUNIT_EXPECTATION,				       \
			     fmt,					       \
			     ##__VA_ARGS__)

/* Helper to safely pass around an initializer list to other macros. */
#define KUNIT_INIT_ASSERT(initializers...) { initializers }

#define KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition_,				       \
			      expected_true_,				       \
			      fmt,					       \
			      ...)					       \
do {									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(!!(condition_) == !!expected_true_))			       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_unary_assert,				       \
		      kunit_unary_assert_format,			       \
		      KUNIT_INIT_ASSERT(.condition = #condition_,	       \
					.expected_true = expected_true_),      \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_TRUE_MSG_ASSERTION(test, assert_type, condition, fmt, ...)       \
	KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition,				       \
			      true,					       \
			      fmt,					       \
			      ##__VA_ARGS__)

#define KUNIT_FALSE_MSG_ASSERTION(test, assert_type, condition, fmt, ...)      \
	KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition,				       \
			      false,					       \
			      fmt,					       \
			      ##__VA_ARGS__)

/*
 * A factory macro for defining the assertions and expectations for the basic
 * comparisons defined for the built in types.
 *
 * Unfortunately, there is no common type that all types can be promoted to for
 * which all the binary operators behave the same way as for the actual types
 * (for example, there is no type that long long and unsigned long long can
 * both be cast to where the comparison result is preserved for all values). So
 * the best we can do is do the comparison in the original types and then coerce
 * everything to long long for printing; this way, the comparison behaves
 * correctly and the printed out value usually makes sense without
 * interpretation, but can always be interpreted to figure out the actual
 * value.
 */
#define KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    assert_class,			       \
				    format_func,			       \
				    assert_type,			       \
				    left,				       \
				    op,					       \
				    right,				       \
				    fmt,				       \
				    ...)				       \
do {									       \
	const typeof(left) __left = (left);				       \
	const typeof(right) __right = (right);				       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(__left op __right))					       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      assert_class,					       \
		      format_func,					       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_BINARY_INT_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				    ...)				       \
	KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    kunit_binary_assert,		       \
				    kunit_binary_assert_format,		       \
				    assert_type,			       \
				    left, op, right,			       \
				    fmt,				       \
				    ##__VA_ARGS__)

#define KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				    ...)				       \
	KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    kunit_binary_ptr_assert,		       \
				    kunit_binary_ptr_assert_format,	       \
				    assert_type,			       \
				    left, op, right,			       \
				    fmt,				       \
				    ##__VA_ARGS__)

#define KUNIT_BINARY_STR_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				   ...)					       \
do {									       \
	const char *__left = (left);					       \
	const char *__right = (right);					       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely((__left) && (__right) && (strcmp(__left, __right) op 0)))   \
		break;							       \
									       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_binary_str_assert,				       \
		      kunit_binary_str_assert_format,			       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_MEM_ASSERTION(test,					       \
			    assert_type,				       \
			    left,					       \
			    op,						       \
			    right,					       \
			    size_,					       \
			    fmt,					       \
			    ...)					       \
do {									       \
	const void *__left = (left);					       \
	const void *__right = (right);					       \
	const size_t __size = (size_);					       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(__left && __right))					       \
		if (likely(memcmp(__left, __right, __size) op 0))	       \
			break;						       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_mem_assert,					       \
		      kunit_mem_assert_format,				       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right,		       \
					.size = __size),		       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						assert_type,		       \
						ptr,			       \
						fmt,			       \
						...)			       \
do {									       \
	const typeof(ptr) __ptr = (ptr);				       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (!IS_ERR_OR_NULL(__ptr))					       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_ptr_not_err_assert,				       \
		      kunit_ptr_not_err_assert_format,			       \
		      KUNIT_INIT_ASSERT(.text = #ptr, .value = __ptr),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

/**
 * KUNIT_EXPECT_TRUE() - Causes a test failure when the expression is not true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to true.
 *
 * This and expectations of the form `KUNIT_EXPECT_*` will cause the test case
 * to fail when the specified condition is not met; however, it will not prevent
 * the test case from continuing to run; this is otherwise known as an
 * *expectation failure*.
 */
#define KUNIT_EXPECT_TRUE(test, condition) \
	KUNIT_EXPECT_TRUE_MSG(test, condition, NULL)

#define KUNIT_EXPECT_TRUE_MSG(test, condition, fmt, ...)		       \
	KUNIT_TRUE_MSG_ASSERTION(test,					       \
				 KUNIT_EXPECTATION,			       \
				 condition,				       \
				 fmt,					       \
				 ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_FALSE() - Makes a test failure when the expression is not false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to false.
 *
 * Sets an expectation that @condition evaluates to false. See
 * KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_FALSE(test, condition) \
	KUNIT_EXPECT_FALSE_MSG(test, condition, NULL)

#define KUNIT_EXPECT_FALSE_MSG(test, condition, fmt, ...)		       \
	KUNIT_FALSE_MSG_ASSERTION(test,					       \
				  KUNIT_EXPECTATION,			       \
				  condition,				       \
				  fmt,					       \
				  ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_EQ() - Sets an expectation that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) == (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_EQ(test, left, right) \
	KUNIT_EXPECT_EQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_PTR_EQ() - Expects that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) == (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_PTR_EQ(test, left, right)				       \
	KUNIT_EXPECT_PTR_EQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_PTR_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NE() - An expectation that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are not
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) != (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_NE(test, left, right) \
	KUNIT_EXPECT_NE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_PTR_NE() - Expects that pointers @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an expectation that the values that @left and @right evaluate to are not
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) != (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_PTR_NE(test, left, right)				       \
	KUNIT_EXPECT_PTR_NE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_PTR_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_LT() - An expectation that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than the
 * value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) < (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_LT(test, left, right) \
	KUNIT_EXPECT_LT_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_LT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, <, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_LE() - Expects that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to. Semantically this is equivalent
 * to KUNIT_EXPECT_TRUE(@test, (@left) <= (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_LE(test, left, right) \
	KUNIT_EXPECT_LE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_LE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, <=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_GT() - An expectation that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) > (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_GT(test, left, right) \
	KUNIT_EXPECT_GT_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_GT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, >, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_GE() - Expects that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) >= (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_GE(test, left, right) \
	KUNIT_EXPECT_GE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_GE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, >=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_STREQ() - Expects that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !strcmp((@left), (@right))). See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_EXPECT_STREQ(test, left, right) \
	KUNIT_EXPECT_STREQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_STREQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_STRNEQ() - Expects that strings @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, strcmp((@left), (@right))). See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_EXPECT_STRNEQ(test, left, right) \
	KUNIT_EXPECT_STRNEQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_STRNEQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_MEMEQ() - Expects that the first @size bytes of @left and @right are equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !memcmp((@left), (@right), (@size))). See
 * KUNIT_EXPECT_TRUE() for more information.
 *
 * Although this expectation works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This expectation is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_EXPECT_MEMEQ(test, left, right, size) \
	KUNIT_EXPECT_MEMEQ_MSG(test, left, right, size, NULL)

#define KUNIT_EXPECT_MEMEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_EXPECTATION,				       \
			    left, ==, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_MEMNEQ() - Expects that the first @size bytes of @left and @right are not equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, memcmp((@left), (@right), (@size))). See
 * KUNIT_EXPECT_TRUE() for more information.
 *
 * Although this expectation works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This expectation is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_EXPECT_MEMNEQ(test, left, right, size) \
	KUNIT_EXPECT_MEMNEQ_MSG(test, left, right, size, NULL)

#define KUNIT_EXPECT_MEMNEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_EXPECTATION,				       \
			    left, !=, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NULL() - Expects that @ptr is null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is null. This is
 * semantically equivalent to KUNIT_EXPECT_PTR_EQ(@test, ptr, NULL).
 * See KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_NULL(test, ptr)				               \
	KUNIT_EXPECT_NULL_MSG(test,					       \
			      ptr,					       \
			      NULL)

#define KUNIT_EXPECT_NULL_MSG(test, ptr, fmt, ...)	                       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   ptr, ==, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NOT_NULL() - Expects that @ptr is not null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is not null. This
 * is semantically equivalent to KUNIT_EXPECT_PTR_NE(@test, ptr, NULL).
 * See KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_NOT_NULL(test, ptr)			               \
	KUNIT_EXPECT_NOT_NULL_MSG(test,					       \
				  ptr,					       \
				  NULL)

#define KUNIT_EXPECT_NOT_NULL_MSG(test, ptr, fmt, ...)	                       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   ptr, !=, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NOT_ERR_OR_NULL() - Expects that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is not null and not
 * an errno stored in a pointer. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !IS_ERR_OR_NULL(@ptr)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_NOT_ERR_OR_NULL(test, ptr) \
	KUNIT_EXPECT_NOT_ERR_OR_NULL_MSG(test, ptr, NULL)

#define KUNIT_EXPECT_NOT_ERR_OR_NULL_MSG(test, ptr, fmt, ...)		       \
	KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						KUNIT_EXPECTATION,	       \
						ptr,			       \
						fmt,			       \
						##__VA_ARGS__)

/**
 * KUNIT_FAIL_AND_ABORT() - Always causes a test to fail and abort when evaluated.
 * @test: The test context object.
 * @fmt: an informational message to be printed when the assertion is made.
 * @...: string format arguments.
 *
 * The opposite of KUNIT_SUCCEED(), it is an assertion that always fails. In
 * other words, it always results in a failed assertion, and consequently
 * always causes the test case to fail and abort when evaluated.
 * See KUNIT_ASSERT_TRUE() for more information.
 */
#define KUNIT_FAIL_AND_ABORT(test, fmt, ...) \
	KUNIT_FAIL_ASSERTION(test, KUNIT_ASSERTION, fmt, ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_TRUE() - Sets an assertion that @condition is true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails and aborts when
 * this does not evaluate to true.
 *
 * This and assertions of the form `KUNIT_ASSERT_*` will cause the test case to
 * fail *and immediately abort* when the specified condition is not met. Unlike
 * an expectation failure, it will prevent the test case from continuing to run;
 * this is otherwise known as an *assertion failure*.
 */
#define KUNIT_ASSERT_TRUE(test, condition) \
	KUNIT_ASSERT_TRUE_MSG(test, condition, NULL)

#define KUNIT_ASSERT_TRUE_MSG(test, condition, fmt, ...)		       \
	KUNIT_TRUE_MSG_ASSERTION(test,					       \
				 KUNIT_ASSERTION,			       \
				 condition,				       \
				 fmt,					       \
				 ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_FALSE() - Sets an assertion that @condition is false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression.
 *
 * Sets an assertion that the value that @condition evaluates to is false. This
 * is the same as KUNIT_EXPECT_FALSE(), except it causes an assertion failure
 * (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_FALSE(test, condition) \
	KUNIT_ASSERT_FALSE_MSG(test, condition, NULL)

#define KUNIT_ASSERT_FALSE_MSG(test, condition, fmt, ...)		       \
	KUNIT_FALSE_MSG_ASSERTION(test,					       \
				  KUNIT_ASSERTION,			       \
				  condition,				       \
				  fmt,					       \
				  ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_EQ() - Sets an assertion that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_EQ(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_EQ(test, left, right) \
	KUNIT_ASSERT_EQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_PTR_EQ() - Asserts that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_EQ(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_PTR_EQ(test, left, right) \
	KUNIT_ASSERT_PTR_EQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_PTR_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NE() - An assertion that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are not
 * equal. This is the same as KUNIT_EXPECT_NE(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NE(test, left, right) \
	KUNIT_ASSERT_NE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_PTR_NE() - Asserts that pointers @left and @right are not equal.
 * KUNIT_ASSERT_PTR_EQ() - Asserts that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an assertion that the values that @left and @right evaluate to are not
 * equal. This is the same as KUNIT_EXPECT_NE(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_PTR_NE(test, left, right) \
	KUNIT_ASSERT_PTR_NE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_PTR_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)
/**
 * KUNIT_ASSERT_LT() - An assertion that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_LT(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_LT(test, left, right) \
	KUNIT_ASSERT_LT_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_LT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, <, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)
/**
 * KUNIT_ASSERT_LE() - An assertion that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to. This is the same as
 * KUNIT_EXPECT_LE(), except it causes an assertion failure (see
 * KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_LE(test, left, right) \
	KUNIT_ASSERT_LE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_LE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, <=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_GT() - An assertion that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_GT(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_GT(test, left, right) \
	KUNIT_ASSERT_GT_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_GT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, >, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_GE() - Assertion that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_GE(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_GE(test, left, right) \
	KUNIT_ASSERT_GE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_GE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, >=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_STREQ() - An assertion that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_STREQ(), except it causes an
 * assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_STREQ(test, left, right) \
	KUNIT_ASSERT_STREQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_STREQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_STRNEQ() - An assertion that strings @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, strcmp((@left), (@right))). See KUNIT_ASSERT_TRUE()
 * for more information.
 */
#define KUNIT_ASSERT_STRNEQ(test, left, right) \
	KUNIT_ASSERT_STRNEQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_STRNEQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_MEMEQ() - Asserts that the first @size bytes of @left and @right are equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, !memcmp((@left), (@right), (@size))). See
 * KUNIT_ASSERT_TRUE() for more information.
 *
 * Although this assertion works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This assertion is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_ASSERT_MEMEQ(test, left, right, size) \
	KUNIT_ASSERT_MEMEQ_MSG(test, left, right, size, NULL)

#define KUNIT_ASSERT_MEMEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_ASSERTION,				       \
			    left, ==, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_MEMNEQ() - Asserts that the first @size bytes of @left and @right are not equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, memcmp((@left), (@right), (@size))). See
 * KUNIT_ASSERT_TRUE() for more information.
 *
 * Although this assertion works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This assertion is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_ASSERT_MEMNEQ(test, left, right, size) \
	KUNIT_ASSERT_MEMNEQ_MSG(test, left, right, size, NULL)

#define KUNIT_ASSERT_MEMNEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_ASSERTION,				       \
			    left, !=, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NULL() - Asserts that pointers @ptr is null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the values that @ptr evaluates to is null. This is
 * the same as KUNIT_EXPECT_NULL(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NULL(test, ptr) \
	KUNIT_ASSERT_NULL_MSG(test,					       \
			      ptr,					       \
			      NULL)

#define KUNIT_ASSERT_NULL_MSG(test, ptr, fmt, ...) \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   ptr, ==, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NOT_NULL() - Asserts that pointers @ptr is not null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the values that @ptr evaluates to is not null. This
 * is the same as KUNIT_EXPECT_NOT_NULL(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NOT_NULL(test, ptr) \
	KUNIT_ASSERT_NOT_NULL_MSG(test,					       \
				  ptr,					       \
				  NULL)

#define KUNIT_ASSERT_NOT_NULL_MSG(test, ptr, fmt, ...) \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   ptr, !=, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NOT_ERR_OR_NULL() - Assertion that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the value that @ptr evaluates to is not null and not
 * an errno stored in a pointer. This is the same as
 * KUNIT_EXPECT_NOT_ERR_OR_NULL(), except it causes an assertion failure (see
 * KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptr) \
	KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ptr, NULL)

#define KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ptr, fmt, ...)		       \
	KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						KUNIT_ASSERTION,	       \
						ptr,			       \
						fmt,			       \
						##__VA_ARGS__)

/**
 * KUNIT_ARRAY_PARAM() - Define test parameter generator from an array.
 * @name:  prefix for the test parameter generator function.
 * @array: array of test parameters.
 * @get_desc: function to convert param to description; NULL to use default
 *
 * Define function @name_gen_params which uses @array to generate parameters.
 */
#define KUNIT_ARRAY_PARAM(name, array, get_desc)						\
	static const void *name##_gen_params(const void *prev, char *desc)			\
	{											\
		typeof((array)[0]) *__next = prev ? ((typeof(__next)) prev) + 1 : (array);	\
		if (__next - (array) < ARRAY_SIZE((array))) {					\
			void (*__get_desc)(typeof(__next), char *) = get_desc;			\
			if (__get_desc)								\
				__get_desc(__next, desc);					\
			return __next;								\
		}										\
		return NULL;									\
	}

/**
 * KUNIT_ARRAY_PARAM_DESC() - Define test parameter generator from an array.
 * @name:  prefix for the test parameter generator function.
 * @array: array of test parameters.
 * @desc_member: structure member from array element to use as description
 *
 * Define function @name_gen_params which uses @array to generate parameters.
 */
#define KUNIT_ARRAY_PARAM_DESC(name, array, desc_member)					\
	static const void *name##_gen_params(const void *prev, char *desc)			\
	{											\
		typeof((array)[0]) *__next = prev ? ((typeof(__next)) prev) + 1 : (array);	\
		if (__next - (array) < ARRAY_SIZE((array))) {					\
			strscpy(desc, __next->desc_member, KUNIT_PARAM_DESC_SIZE);		\
			return __next;								\
		}										\
		return NULL;									\
	}

// TODO(dlatypov@google.com): consider eventually migrating users to explicitly
// include resource.h themselves if they need it.
#include <kunit/resource.h>

#endif /* _KUNIT_TEST_H */
