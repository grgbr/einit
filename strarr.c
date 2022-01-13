#include "strarr.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

char *
strrep(const char * orig, size_t len)
{
	assert(orig);

	char * str;

	str = malloc(len + 1);
	if (!str)
		return NULL;

	memcpy(str, orig, len);
	str[len] = '\0';

	return str;
}

int
strarr_rep(struct strarr * array,
           unsigned int             index,
           const char *    orig,
           size_t                   len)
{
	assert(array);
	assert(array->nr);
	assert(index < array->nr);
	assert(orig);

	char * str;

	str = strrep(orig, len);
	if (!str)
		return -errno;

	array->strings[index] = str;

	return 0;
}

struct strarr *
strarr_create(unsigned int nr)
{
	assert(nr);

	struct strarr * arr;
	size_t          sz = sizeof(*arr) + (nr * sizeof(arr->strings[0]));

	arr = malloc(sz);
	if (!arr)
		return NULL;

	memset(arr, 0, sz);

	arr->nr = nr;

	return arr;
}

void
strarr_destroy(struct strarr * array)
{
	assert(array);
	assert(array->nr);

	unsigned int s;

	for (s = 0; (s < array->nr) && array->strings[s]; s++)
		free((void *)array->strings[s]);

	free(array);
}
