#include "hammer2.h"

static int docleanup(const char *path);
static int sameh2prefix(const char *from);
static void saveh2prefix(const char *from);
static void freeh2prefixes(void);

int
cmd_cleanup(const char *sel_path)
{

	char *fstype;
	char *path;
	char *from;
	int i;
	int n;
	int r;
	int rc;

	if (sel_path)
		return (docleanup(sel_path));

	n = -1;
	if (n <= 0) {
		fprintf(stderr, "hammer2 cleanup: no HAMMER2 mounts\n");
		return 0;
	}
	rc = 0;
	for (i = 0; i < n; ++i) {

		fstype = path = from = NULL;

		if (strcmp(fstype, "hammer2") != 0)
			continue;
		if (sameh2prefix(from)) {
			printf("hammer2 cleanup \"%s\" (same partition)\n",
				path);
		} else {
			r = docleanup(path);
			if (r)
				rc = r;
			saveh2prefix(from);
		}
	}
	freeh2prefixes();

	return rc;
}

static
int
docleanup(const char *path)
{
	int rc;

	printf("hammer2 cleanup \"%s\"\n", path);
	rc = cmd_bulkfree(path);

	return rc;
}

static char **h2prefixes;
static int h2count;
static int h2limit;

static
int
sameh2prefix(const char *from)
{
	char *ptr;
	int rc = 0;
	int i;

	ptr = strdup(from);
	if (strchr(ptr, '@'))
		*strchr(ptr, '@') = 0;
	for (i = 0; i < h2count; ++i) {
		if (strcmp(h2prefixes[i], ptr) == 0) {
			rc = 1;
			break;
		}
	}
	free(ptr);

	return rc;
}

static
void
saveh2prefix(const char *from)
{
	char *ptr;

	if (h2count >= h2limit) {
		h2limit = (h2limit + 8) << 1;
		h2prefixes = realloc(h2prefixes, sizeof(char *) * h2limit);
	}
	ptr = strdup(from);
	if (strchr(ptr, '@'))
		*strchr(ptr, '@') = 0;
	h2prefixes[h2count++] = ptr;
}

static
void
freeh2prefixes(void)
{
	int i;

	for (i = 0; i < h2count; ++i)
		free(h2prefixes[i]);
	free(h2prefixes);
	h2prefixes = NULL;
	h2count = 0;
	h2limit = 0;
}
