#ifndef _TINIT_STRING_H
#define _TINIT_STRING_H

#include <assert.h>
#include <sys/types.h>

/*
 * strrep() - Replicate a string.
 *
 * @orig: string to replicate
 * @len:  length of string to replicate (without terminating NULL byte)
 *
 * Allocate a new @len + 1 bytes long string, copy @len bytes of the original
 * string into it, then end it with a terminating NULL byte.
 *
 * Return: the newly replicated string or NULL in case of error.
 */
extern char *
strrep(const char * orig, size_t len);

/*
 * struct strarr - A fixed sized array to store malloc'ed strings.
 */
struct strarr {
	unsigned int nr;
	const char * strings[];
};

static inline unsigned int
strarr_nr(const struct strarr * array)
{
	assert(array);
	assert(array->nr);

	return array->nr;
}

static inline const char *
strarr_get(const struct strarr * array, unsigned int index)
{
	assert(array);
	assert(index < strarr_nr(array));

	return array->strings[index];
}

static inline void
strarr_put(struct strarr * array, unsigned int index, const char * string)
{
	assert(array);
	assert(index < strarr_nr(array));

	array->strings[index] = string;
}

static inline const char * const *
strarr_get_members(const struct strarr * array)
{
	assert(array);
	assert(array->nr);

	return array->strings;
}

/*
 * strarr_rep() - Replicate a string and insert it into a malloc'ed string
 *                array.
 * 
 * @array: a malloc'ed string array
 * @index: index identifying the array slot to insert to newly replicated string
 *         into
 * @orig:  the original string to replicate
 * @len:   length in bytes (without terminating NULL byte) of the original
 *         string to replicate
 *
 * Return:  0 - success,
 *         <0 - an errno like negative error code
 */
extern int strarr_rep(struct strarr * array,
                      unsigned int             index,
                      const char *    orig,
                      size_t                   len);

/*
 * strarr_create() - Create an array to store malloc'ed strings.
 * 
 * @nr: maximum number of array slots
 *
 * Return: >0   - address of the malloc'ed string array,
 *         NULL - error (with errno set appropriately)
 */
extern struct strarr * strarr_create(unsigned int nr);

/*
 * strarr_destroy() - Release resources allocated for a replicated strings
 *                    array.
 * @array: a replicated strings array
 *
 * Will free memory allocated for the given array and all of its string
 * content.
 */
extern void strarr_destroy(struct strarr * array);

#endif /* _TINIT_STRING_H */
