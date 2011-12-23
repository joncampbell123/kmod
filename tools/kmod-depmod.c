/*
 * kmod-depmod - calculate modules.dep  using libkmod.
 *
 * Copyright (C) 2011  ProFUSION embedded systems
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <limits.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <regex.h>
#include <assert.h>
#include <unistd.h>
#include "libkmod.h"

#define streq(a, b) (strcmp(a, b) == 0)
#define strstartswith(a, b) (strncmp(a, b, strlen(b)) == 0)

#define DEFAULT_VERBOSE LOG_WARNING
static int verbose = DEFAULT_VERBOSE;

static const char CFG_BUILTIN_KEY[] = "built-in";
static const char *default_cfg_paths[] = {
	"/run/depmod.d",
	SYSCONFDIR "/depmod.d",
	ROOTPREFIX "/lib/depmod.d",
	NULL
};

static const char cmdopts_s[] = "aAb:C:E:F:euqrvnP:wmVh";
static const struct option cmdopts[] = {
	{"all", no_argument, 0, 'a'},
	{"quick", no_argument, 0, 'A'},
	{"basedir", required_argument, 0, 'b'},
	{"config", required_argument, 0, 'C'},
	{"symvers", required_argument, 0, 'E'},
	{"filesyms", required_argument, 0, 'F'},
	{"errsyms", no_argument, 0, 'e'},
	{"unresolved-error", no_argument, 0, 'u'}, /* deprecated */
	{"quiet", no_argument, 0, 'q'}, /* deprecated */
	{"root", no_argument, 0, 'r'}, /* deprecated */
	{"verbose", no_argument, 0, 'v'},
	{"show", no_argument, 0, 'n'},
	{"dry-run", no_argument, 0, 'n'},
	{"symbol-prefix", no_argument, 0, 'P'},
	{"warn", no_argument, 0, 'w'},
	{"map", no_argument, 0, 'm'}, /* deprecated */
	{"version", no_argument, 0, 'V'},
	{"help", no_argument, 0, 'h'},
	{NULL, 0, 0, 0}
};

static void help(const char *progname)
{
	fprintf(stderr,
		"Usage:\n"
		"\t%s -[aA] [options] [forced_version]\n"
		"\n"
		"If no arguments (except options) are given, \"depmod -a\" is assumed\n"
		"\n"
		"depmod will output a dependency list suitable for the modprobe utility.\n"
		"\n"
		"Options:\n"
		"\t-a, --all            Probe all modules\n"
		"\t-A, --quick          Only does the work if there's a new module\n"
		"\t-e, --errsyms        Report not supplied symbols\n"
		"\t-n, --show           Write the dependency file on stdout only\n"
		"\t-P, --symbol-prefix  Architecture symbol prefix\n"
		"\t-v, --verbose        Enable verbose mode\n"
		"\t-w, --warn           Warn on duplicates\n"
		"\t-V, --version        show version\n"
		"\t-h, --help           show this help\n"
		"\n"
		"The following options are useful for people managing distributions:\n"
		"\t-b, --basedir=DIR    Use an image of a module tree.\n"
		"\t-F, --filesyms=FILE  Use the file instead of the\n"
		"\t                     current kernel symbols.\n"
		"\t-E, --symvers=FILE   Use Module.symvers file to check\n"
		"\t                     symbol versions.\n",
		progname);
}

static inline void _show(const char *fmt, ...)
{
	va_list args;

	if (verbose <= DEFAULT_VERBOSE)
		return;

	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	fflush(stdout);
	va_end(args);
}

static inline void _log(int prio, const char *fmt, ...)
{
	const char *prioname;
	char buf[32], *msg;
	va_list args;

	if (prio > verbose)
		return;

	va_start(args, fmt);
	if (vasprintf(&msg, fmt, args) < 0)
		msg = NULL;
	va_end(args);
	if (msg == NULL)
		return;

	switch (prio) {
	case LOG_CRIT:
		prioname = "FATAL";
		break;
	case LOG_ERR:
		prioname = "ERROR";
		break;
	case LOG_WARNING:
		prioname = "WARNING";
		break;
	case LOG_NOTICE:
		prioname = "NOTICE";
		break;
	case LOG_INFO:
		prioname = "INFO";
		break;
	case LOG_DEBUG:
		prioname = "DEBUG";
		break;
	default:
		snprintf(buf, sizeof(buf), "LOG-%03d", prio);
		prioname = buf;
	}

	fprintf(stderr, "%s: %s", prioname, msg);
	free(msg);

	if (prio <= LOG_CRIT)
		exit(EXIT_FAILURE);
}
#define CRIT(...) _log(LOG_CRIT, __VA_ARGS__)
#define ERR(...) _log(LOG_ERR, __VA_ARGS__)
#define WRN(...) _log(LOG_WARNING, __VA_ARGS__)
#define INF(...) _log(LOG_INFO, __VA_ARGS__)
#define DBG(...) _log(LOG_DEBUG, __VA_ARGS__)
#define SHOW(...) _show(__VA_ARGS__)


/* hash like libkmod-hash.c *******************************************/
struct hash_entry {
	const char *key;
	const void *value;
};

struct hash_bucket {
	struct hash_entry *entries;
	unsigned int used;
	unsigned int total;
};

struct hash {
	unsigned int count;
	unsigned int step;
	unsigned int n_buckets;
	void (*free_value)(void *value);
	struct hash_bucket buckets[];
};

static struct hash *hash_new(unsigned int n_buckets,
				void (*free_value)(void *value))
{
	struct hash *hash = calloc(1, sizeof(struct hash) +
				n_buckets * sizeof(struct hash_bucket));
	if (hash == NULL)
		return NULL;
	hash->n_buckets = n_buckets;
	hash->free_value = free_value;
	hash->step = n_buckets / 32;
	if (hash->step == 0)
		hash->step = 4;
	else if (hash->step > 64)
		hash->step = 64;
	return hash;
}

static void hash_free(struct hash *hash)
{
	struct hash_bucket *bucket, *bucket_end;
	bucket = hash->buckets;
	bucket_end = bucket + hash->n_buckets;
	for (; bucket < bucket_end; bucket++) {
		if (hash->free_value) {
			struct hash_entry *entry, *entry_end;
			entry = bucket->entries;
			entry_end = entry + bucket->used;
			for (; entry < entry_end; entry++)
				hash->free_value((void *)entry->value);
		}
		free(bucket->entries);
	}
	free(hash);
}

static inline unsigned int hash_superfast(const char *key, unsigned int len)
{
	/* Paul Hsieh (http://www.azillionmonkeys.com/qed/hash.html)
	 * used by WebCore (http://webkit.org/blog/8/hashtables-part-2/)
	 * EFL's eina and possible others.
	 */
	unsigned int tmp, hash = len, rem = len & 3;
	const unsigned short *itr = (const unsigned short *)key;

	len /= 4;

	/* Main loop */
	for (; len > 0; len--) {
		hash += itr[0];
		tmp = (itr[1] << 11) ^ hash;
		hash = (hash << 16) ^ tmp;
		itr += 2;
		hash += hash >> 11;
	}

	/* Handle end cases */
	switch (rem) {
	case 3:
		hash += *itr;
		hash ^= hash << 16;
		hash ^= key[2] << 18;
		hash += hash >> 11;
		break;

	case 2:
		hash += *itr;
		hash ^= hash << 11;
		hash += hash >> 17;
		break;

	case 1:
		hash += *(const char *)itr;
		hash ^= hash << 10;
		hash += hash >> 1;
	}

	/* Force "avalanching" of final 127 bits */
	hash ^= hash << 3;
	hash += hash >> 5;
	hash ^= hash << 4;
	hash += hash >> 17;
	hash ^= hash << 25;
	hash += hash >> 6;

	return hash;
}

/*
 * add or replace key in hash map.
 *
 * none of key or value are copied, just references are remembered as is,
 * make sure they are live while pair exists in hash!
 */
static int hash_add(struct hash *hash, const char *key, const void *value)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval % hash->n_buckets;
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;

	if (bucket->used + 1 >= bucket->total) {
		unsigned new_total = bucket->total + hash->step;
		size_t size = new_total * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp == NULL)
			return -errno;
		bucket->entries = tmp;
		bucket->total = new_total;
	}

	entry = bucket->entries;
	entry_end = entry + bucket->used;
	for (; entry < entry_end; entry++) {
		int c = strcmp(key, entry->key);
		if (c == 0) {
			hash->free_value((void *)entry->value);
			entry->value = value;
			return 0;
		} else if (c < 0) {
			memmove(entry + 1, entry,
				(entry_end - entry) * sizeof(struct hash_entry));
			break;
		}
	}

	entry->key = key;
	entry->value = value;
	bucket->used++;
	hash->count++;
	return 0;
}

/* similar to hash_add(), but fails if key already exists */
static int hash_add_unique(struct hash *hash, const char *key, const void *value)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval % hash->n_buckets;
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;

	if (bucket->used + 1 >= bucket->total) {
		unsigned new_total = bucket->total + hash->step;
		size_t size = new_total * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp == NULL)
			return -errno;
		bucket->entries = tmp;
		bucket->total = new_total;
	}

	entry = bucket->entries;
	entry_end = entry + bucket->used;
	for (; entry < entry_end; entry++) {
		int c = strcmp(key, entry->key);
		if (c == 0)
			return -EEXIST;
		else if (c < 0) {
			memmove(entry + 1, entry,
				(entry_end - entry) * sizeof(struct hash_entry));
			break;
		}
	}

	entry->key = key;
	entry->value = value;
	bucket->used++;
	hash->count++;
	return 0;
}

static int hash_entry_cmp(const void *pa, const void *pb)
{
	const struct hash_entry *a = pa;
	const struct hash_entry *b = pb;
	return strcmp(a->key, b->key);
}

static void *hash_find(const struct hash *hash, const char *key)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval % hash->n_buckets;
	const struct hash_bucket *bucket = hash->buckets + pos;
	const struct hash_entry se = {
		.key = key,
		.value = NULL
	};
	const struct hash_entry *entry = bsearch(
		&se, bucket->entries, bucket->used,
		sizeof(struct hash_entry), hash_entry_cmp);
	if (entry == NULL)
		return NULL;
	return (void *)entry->value;
}

static int hash_del(struct hash *hash, const char *key)
{
	unsigned int keylen = strlen(key);
	unsigned int hashval = hash_superfast(key, keylen);
	unsigned int pos = hashval % hash->n_buckets;
	unsigned int steps_used, steps_total;
	struct hash_bucket *bucket = hash->buckets + pos;
	struct hash_entry *entry, *entry_end;
	const struct hash_entry se = {
		.key = key,
		.value = NULL
	};

	entry = bsearch(&se, bucket->entries, bucket->used,
		sizeof(struct hash_entry), hash_entry_cmp);
	if (entry == NULL)
		return -ENOENT;

	entry_end = bucket->entries + bucket->used;
	memmove(entry, entry + 1,
		(entry_end - entry) * sizeof(struct hash_entry));

	bucket->used--;
	hash->count--;

	steps_used = bucket->used / hash->step;
	steps_total = bucket->total / hash->step;
	if (steps_used + 1 < steps_total) {
		size_t size = (steps_used + 1) *
			hash->step * sizeof(struct hash_entry);
		struct hash_entry *tmp = realloc(bucket->entries, size);
		if (tmp) {
			bucket->entries = tmp;
			bucket->total = (steps_used + 1) * hash->step;
		}
	}

	return 0;
}

static unsigned int hash_get_count(const struct hash *hash)
{
	return hash->count;
}

/* basic pointer array growing in steps  ******************************/
struct array {
	void **array;
	size_t count;
	size_t total;
	size_t step;
};

static void array_init(struct array *array, size_t step)
{
	assert(step > 0);
	array->array = NULL;
	array->count = 0;
	array->total = 0;
	array->step = step;
}

static int array_append(struct array *array, const void *element)
{
	size_t idx;

	if (array->count + 1 >= array->total) {
		size_t new_total = array->total + array->step;
		void *tmp = realloc(array->array, sizeof(void *) * new_total);
		assert(array->step > 0);
		if (tmp == NULL)
			return -ENOMEM;
		array->array = tmp;
		array->total = new_total;
	}
	idx = array->count;
	array->array[idx] = (void *)element;
	array->count++;
	return idx;
}

static int array_append_unique(struct array *array, const void *element)
{
	void **itr = array->array;
	void **itr_end = itr + array->count;
	for (; itr < itr_end; itr++)
		if (*itr == element)
			return -EEXIST;
	return array_append(array, element);
}

static void array_pop(struct array *array) {
	array->count--;
	if (array->count + array->step < array->total) {
		size_t new_total = array->total - array->step;
		void *tmp = realloc(array->array, sizeof(void *) * new_total);
		assert(array->step > 0);
		if (tmp == NULL)
			return;
		array->array = tmp;
		array->total = new_total;
	}
}

static void array_free_array(struct array *array) {
	free(array->array);
	array->count = 0;
	array->total = 0;
}


static void array_sort(struct array *array, int (*cmp)(const void *a, const void *b))
{
	qsort(array->array, array->count, sizeof(void *), cmp);
}

/* configuration parsing **********************************************/
struct cfg_override {
	struct cfg_override *next;
	size_t len;
	char path[];
};

struct cfg_search {
	struct cfg_search *next;
	uint8_t builtin;
	size_t len;
	char path[];
};

struct cfg {
	const char *kversion;
	char dirname[PATH_MAX];
	size_t dirnamelen;
	char sym_prefix;
	uint8_t check_symvers;
	uint8_t print_unknown;
	uint8_t warn_dups;
	struct cfg_override *overrides;
	struct cfg_search *searches;
};

static int cfg_search_add(struct cfg *cfg, const char *path, uint8_t builtin)
{
	struct cfg_search *s;
	size_t len;

	if (builtin)
		len = 0;
	else
		len = strlen(path) + 1;

	s = malloc(sizeof(struct cfg_search) + len);
	if (s == NULL) {
		ERR("search add: out of memory\n");
		return -ENOMEM;
	}
	s->builtin = builtin;
	if (builtin)
		s->len = 0;
	else {
		s->len = len;
		memcpy(s->path, path, len);
	}

	DBG("search add: %s, builtin=%hhu\n", path, builtin);

	s->next = cfg->searches;
	cfg->searches = s;
	return 0;
}

static void cfg_search_free(struct cfg_search *s)
{
	free(s);
}

static int cfg_override_add(struct cfg *cfg, const char *modname, const char *subdir)
{
	struct cfg_override *o;
	size_t modnamelen = strlen(modname);
	size_t subdirlen = strlen(subdir);
	size_t i;

	o = malloc(sizeof(struct cfg_override) + cfg->dirnamelen + 1 +
		   subdirlen + 1 + modnamelen + 1);
	if (o == NULL) {
		ERR("override add: out of memory\n");
		return -ENOMEM;
	}
	memcpy(o->path, cfg->dirname, cfg->dirnamelen);
	i = cfg->dirnamelen;
	o->path[i] = '/';
	i++;

	memcpy(o->path + i, subdir, subdirlen);
	i += subdirlen;
	o->path[i] = '/';
	i++;

	memcpy(o->path + i, modname, modnamelen);
	i += modnamelen;
	o->path[i] = '\0'; /* no extension, so we can match .ko/.ko.gz */

	o->len = i;

	DBG("override add: %s\n", o->path);

	o->next = cfg->overrides;
	cfg->overrides = o;
	return 0;
}

static void cfg_override_free(struct cfg_override *o)
{
	free(o);
}

static int cfg_kernel_matches(const struct cfg *cfg, const char *pattern)
{
	regex_t re;
	int status;

	/* old style */
	if (streq(pattern, "*"))
		return 1;

	if (regcomp(&re, pattern, REG_EXTENDED|REG_NOSUB) != 0)
		return 0;

	status = regexec(&re, cfg->kversion, 0, NULL, 0);
	regfree(&re);

	return status == 0;
}

/* same as libkmod-util.c */
static char *getline_wrapped(FILE *fp, unsigned int *linenum)
{
	int size = 256;
	int i = 0;
	char *buf = malloc(size);

	for(;;) {
		int ch = getc_unlocked(fp);

		switch(ch) {
		case EOF:
			if (i == 0) {
				free(buf);
				return NULL;
			}
			/* else fall through */

		case '\n':
			if (linenum)
				(*linenum)++;
			if (i == size)
				buf = realloc(buf, size + 1);
			buf[i] = '\0';
			return buf;

		case '\\':
			ch = getc_unlocked(fp);

			if (ch == '\n') {
				if (linenum)
					(*linenum)++;
				continue;
			}
			/* else fall through */

		default:
			buf[i++] = ch;

			if (i == size) {
				size *= 2;
				buf = realloc(buf, size);
			}
		}
	}
}

static int cfg_file_parse(struct cfg *cfg, const char *filename)
{
	char *line;
	FILE *fp;
	unsigned int linenum = 0;
	int err;

	fp = fopen(filename, "r");
	if (fp == NULL) {
		err = -errno;
		ERR("file parse %s: %m", filename);
		return err;
	}

	while ((line = getline_wrapped(fp, &linenum)) != NULL) {
		char *cmd, *saveptr;

		if (line[0] == '\0' || line[0] == '#')
			goto done_next;

		cmd = strtok_r(line, "\t ", &saveptr);
		if (cmd == NULL)
			goto done_next;

		if (streq(cmd, "search")) {
			const char *sp;
			while ((sp = strtok_r(NULL, "\t ", &saveptr)) != NULL) {
				uint8_t builtin = streq(sp, CFG_BUILTIN_KEY);
				cfg_search_add(cfg, sp, builtin);
			}
		} else if (streq(cmd, "override")) {
			const char *modname = strtok_r(NULL, "\t ", &saveptr);
			const char *version = strtok_r(NULL, "\t ", &saveptr);
			const char *subdir = strtok_r(NULL, "\t ", &saveptr);

			if (modname == NULL || version == NULL ||
					subdir == NULL)
				goto syntax_error;

			if (!cfg_kernel_matches(cfg, version)) {
				INF("%s:%u: override kernel did not match %s\n",
				    filename, linenum, version);
				goto done_next;
			}

			cfg_override_add(cfg, modname, subdir);
		} else if (streq(cmd, "include")
				|| streq(cmd, "make_map_files")) {
			INF("%s:%u: command %s not implemented yet\n",
			    filename, linenum, cmd);
		} else {
syntax_error:
			ERR("%s:%u: ignoring bad line starting with '%s'\n",
			    filename, linenum, cmd);
		}

done_next:
		free(line);
	}

	fclose(fp);

	return 0;
}

static int cfg_files_filter_out(DIR *d, const char *dir, const char *name)
{
	size_t len = strlen(name);
	struct stat st;

	if (name[0] == '.')
		return 1;

	if (len < 6 || !streq(name + len - 5, ".conf")) {
		INF("All cfg files need .conf: %s/%s\n", dir, name);
		return 1;
	}

	fstatat(dirfd(d), name, &st, 0);
	if (S_ISDIR(st.st_mode)) {
		ERR("Directories inside directories are not supported: %s/%s\n",
		    dir, name);
		return 1;
	}

	return 0;
}

struct cfg_file {
	size_t dirlen;
	size_t namelen;
	const char *name;
	char path[];
};

static void cfg_file_free(struct cfg_file *f)
{
	free(f);
}

static int cfg_files_insert_sorted(struct cfg_file ***p_files, size_t *p_n_files,
					const char *dir, const char *name)
{
	struct cfg_file **files, *f;
	size_t i, n_files, namelen, dirlen;
	void *tmp;

	dirlen = strlen(dir);
	if (name != NULL)
		namelen = strlen(name);
	else {
		name = basename(dir);
		namelen = strlen(name);
		dirlen -= namelen - 1;
	}

	n_files = *p_n_files;
	files = *p_files;
	for (i = 0; i < n_files; i++) {
		int cmp = strcmp(name, files[i]->name);
		if (cmp == 0) {
			DBG("Ignoring duplicate config file: %.*s/%s\n",
			    (int)dirlen, dir, name);
			return -EEXIST;
		} else if (cmp < 0)
			break;
	}

	f = malloc(sizeof(struct cfg_file) + dirlen + namelen + 2);
	if (f == NULL) {
		ERR("files insert sorted: out of memory\n");
		return -ENOMEM;
	}

	tmp = realloc(files, sizeof(struct cfg_file *) * (n_files + 1));
	if (tmp == NULL) {
		ERR("files insert sorted: out of memory\n");
		free(f);
		return -ENOMEM;
	}
	*p_files = files = tmp;

	if (i < n_files) {
		memmove(files + i + 1, files + i,
			sizeof(struct cfg_file *) * (n_files - i));
	}
	files[i] = f;

	f->dirlen = dirlen;
	f->namelen = namelen;
	f->name = f->path + dirlen + 1;
	memcpy(f->path, dir, dirlen);
	f->path[dirlen] = '/';
	memcpy(f->path + dirlen + 1, name, namelen);
	f->path[dirlen + 1 + namelen] = '\0';

	*p_n_files = n_files + 1;
	return 0;
}

/*
 * Insert configuration files ignoring duplicates
 */
static int cfg_files_list(struct cfg_file ***p_files, size_t *p_n_files,
				const char *path)
{
	DIR *d;
	int err = 0;
	struct stat st;

	if (stat(path, &st) != 0) {
		err = -errno;
		DBG("could not stat '%s': %m\n", path);
		return err;
	}

	if (S_ISREG(st.st_mode)) {
		cfg_files_insert_sorted(p_files, p_n_files, path, NULL);
		return 0;
	} if (!S_ISDIR(st.st_mode)) {
		ERR("unsupported file mode %s: %#x\n", path, st.st_mode);
		return -EINVAL;
	}

	d = opendir(path);
	if (d == NULL) {
		ERR("files list %s: %m\n", path);
		return -EINVAL;
	}

	for (;;) {
		struct dirent ent, *entp;

		err = readdir_r(d, &ent, &entp);
		if (err != 0) {
			ERR("reading entry %s\n", strerror(-err));
			break;
		}
		if (entp == NULL)
			break;
		if (cfg_files_filter_out(d, path, entp->d_name))
			continue;

		cfg_files_insert_sorted(p_files, p_n_files, path, entp->d_name);
	}

	closedir(d);
	DBG("parsed configuration files from %s: %s\n", path, strerror(-err));
	return err;
}

static int cfg_load(struct cfg *cfg, const char * const *cfg_paths)
{
	size_t i, n_files = 0;
	struct cfg_file **files = NULL;

	if (cfg_paths == NULL)
		cfg_paths = default_cfg_paths;

	for (i = 0; cfg_paths[i] != NULL; i++)
		cfg_files_list(&files, &n_files, cfg_paths[i]);

	for (i = 0; i < n_files; i++) {
		struct cfg_file *f = files[i];
		cfg_file_parse(cfg, f->path);
		cfg_file_free(f);
	}
	free(files);

	/* For backward compatibility add "updates" to the head of the search
	 * list here. But only if there was no "search" option specified.
	 */
	if (cfg->searches == NULL)
		cfg_search_add(cfg, "updates", 0);

	return 0;
}

static void cfg_free(struct cfg *cfg)
{
	while (cfg->overrides) {
		struct cfg_override *tmp = cfg->overrides;
		cfg->overrides = cfg->overrides->next;
		cfg_override_free(tmp);
	}

	while (cfg->searches) {
		struct cfg_search *tmp = cfg->searches;
		cfg->searches = cfg->searches->next;
		cfg_search_free(tmp);
	}
}


/* depmod calculations ***********************************************/
struct mod {
	struct kmod_module *kmod;
	const char *path;
	const char *relpath; /* path relative to '$ROOT/lib/modules/$VER/' */
	struct array deps; /* struct symbol */
	size_t baselen; /* points to start of basename/filename */
	size_t modnamelen;
	int sort_idx; /* sort index using modules.order */
	int dep_sort_idx; /* topological sort index */
	uint16_t idx; /* index in depmod->modules.array */
	uint16_t users; /* how many modules depend on this one */
	uint8_t dep_loop : 1;
	char modname[];
};

struct symbol {
	struct mod *owner;
	uint64_t crc;
	char name[];
};

struct depmod {
	const struct cfg *cfg;
	struct kmod_ctx *ctx;
	struct array modules;
	struct hash *modules_by_relpath;
	struct hash *modules_by_name;
	struct hash *symbols;
	unsigned int dep_loops;
};

static void mod_free(struct mod *mod)
{
	DBG("free %p kmod=%p, path=%s\n", mod, mod->kmod, mod->path);
	array_free_array(&mod->deps);
	kmod_module_unref(mod->kmod);
	free(mod);
}

static int mod_add_dependency(struct mod *mod, struct symbol *sym)
{
	int err;

	DBG("%s depends on %s %s\n", mod->path, sym->name,
	    sym->owner != NULL ? sym->owner->path : "(unknown)");

	if (sym->owner == NULL)
		return 0;

	err = array_append_unique(&mod->deps, sym->owner);
	if (err == -EEXIST)
		return 0;
	if (err < 0)
		return err;

	sym->owner->users++;
	SHOW("%s needs \"%s\": %s\n", mod->path, sym->name, sym->owner->path);
	return 0;
}

static void symbol_free(struct symbol *sym)
{
	DBG("free %p sym=%s, owner=%p %s\n", sym, sym->name, sym->owner,
	    sym->owner != NULL ? sym->owner->path : "");
	free(sym);
}

static int depmod_init(struct depmod *depmod, struct cfg *cfg, struct kmod_ctx *ctx)
{
	int err = 0;

	depmod->cfg = cfg;
	depmod->ctx = ctx;

	array_init(&depmod->modules, 128);

	depmod->modules_by_relpath = hash_new(512, NULL);
	if (depmod->modules_by_relpath == NULL) {
		err = -errno;
		goto modules_by_relpath_failed;
	}

	depmod->modules_by_name = hash_new(512, NULL);
	if (depmod->modules_by_name == NULL) {
		err = -errno;
		goto modules_by_name_failed;
	}

	depmod->symbols = hash_new(2048, (void (*)(void *))symbol_free);
	if (depmod->symbols == NULL) {
		err = -errno;
		goto symbols_failed;
	}

	return 0;

symbols_failed:
	hash_free(depmod->modules_by_name);
modules_by_name_failed:
	hash_free(depmod->modules_by_relpath);
modules_by_relpath_failed:
	return err;
}

static void depmod_shutdown(struct depmod *depmod)
{
	size_t i;

	hash_free(depmod->symbols);

	hash_free(depmod->modules_by_relpath);

	hash_free(depmod->modules_by_name);

	for (i = 0; i < depmod->modules.count; i++)
		mod_free(depmod->modules.array[i]);
	array_free_array(&depmod->modules);

	kmod_unref(depmod->ctx);
}

static int depmod_module_add(struct depmod *depmod, struct kmod_module *kmod)
{
	const struct cfg *cfg = depmod->cfg;
	const char *modname;
	size_t modnamelen;
	struct mod *mod;
	int err;

	modname = kmod_module_get_name(kmod);
	modnamelen = strlen(modname) + 1;

	mod = calloc(1, sizeof(struct mod) + modnamelen);
	if (mod == NULL)
		return -ENOMEM;
	mod->kmod = kmod;
	mod->sort_idx = depmod->modules.count + 1;
	mod->dep_sort_idx = INT32_MAX;
	mod->idx = depmod->modules.count;
	memcpy(mod->modname, modname, modnamelen);
	mod->modnamelen = modnamelen;

	array_init(&mod->deps, 4);

	mod->path = kmod_module_get_path(kmod);
	mod->baselen = strrchr(mod->path, '/') - mod->path + 1;
	if (strncmp(mod->path, cfg->dirname, cfg->dirnamelen) == 0 &&
			mod->path[cfg->dirnamelen] == '/')
		mod->relpath = mod->path + cfg->dirnamelen + 1;
	else
		mod->relpath = NULL;

	err = array_append(&depmod->modules, mod);
	if (err < 0) {
		free(mod);
		return err;
	}

	err = hash_add_unique(depmod->modules_by_name, mod->modname, mod);
	if (err < 0) {
		ERR("hash_add_unique %s: %s\n", mod->modname, strerror(-err));
		array_pop(&depmod->modules);
		free(mod);
		return err;
	}

	if (mod->relpath != NULL) {
		err = hash_add_unique(depmod->modules_by_relpath,
				      mod->relpath, mod);
		if (err < 0) {
			ERR("hash_add_unique %s: %s\n",
			    mod->relpath, strerror(-err));
			hash_del(depmod->modules_by_name, mod->modname);
			array_pop(&depmod->modules);
			free(mod);
			return err;
		}
	}

	DBG("add %p kmod=%p, path=%s\n", mod, kmod, mod->path);

	return 0;
}

static int depmod_module_replace(struct depmod *depmod, struct mod *mod, struct kmod_module *kmod)
{
	const struct cfg *cfg = depmod->cfg;
	const char *path, *relpath;
	int err;

	path = kmod_module_get_path(kmod);
	if (strncmp(path, cfg->dirname, cfg->dirnamelen) == 0 &&
			path[cfg->dirnamelen] == '/')
		relpath = path + cfg->dirnamelen + 1;
	else
		relpath = NULL;

	if (relpath != NULL) {
		err = hash_add_unique(depmod->modules_by_relpath, relpath, mod);
		if (err < 0) {
			ERR("hash_add_unique %s: %s\n",
			    relpath, strerror(-err));
			return err;
		}
	}

	if (mod->relpath != NULL)
		hash_del(depmod->modules_by_relpath, mod->relpath);
	kmod_module_unref(mod->kmod);
	mod->relpath = relpath;
	mod->path = path;
	mod->kmod = kmod;
	return 0;
}

/* returns if existing module @mod is higher priority than newpath.
 * note this is the inverse of module-init-tools is_higher_priority()
 */
static int depmod_module_is_higher_priority(const struct depmod *depmod, const struct mod *mod, size_t baselen, size_t namelen, size_t modnamelen, const char *newpath)
{
	const struct cfg *cfg = depmod->cfg;
	const struct cfg_override *ov;
	const struct cfg_search *se;
	size_t newlen = baselen + modnamelen;
	size_t oldlen = mod->baselen + mod->modnamelen;
	const char *oldpath = mod->path;
	int i, bprio = -1, oldprio = -1, newprio = -1;

	DBG("comparing priorities of %s and %s\n",
	    oldpath, newpath);

	for (ov = cfg->overrides; ov != NULL; ov = ov->next) {
		DBG("override %s\n", ov->path);
		if (newlen == ov->len && memcmp(ov->path, newpath, newlen) == 0)
			return 0;
		if (oldlen == ov->len && memcmp(ov->path, oldpath, oldlen) == 0)
			return 1;
	}

	for (i = 0, se = cfg->searches; se != NULL; se = se->next, i++) {
		DBG("search %s\n", se->builtin ? "built-in" : se->path);
		if (se->builtin)
			bprio = i;
		else if (newlen >= se->len &&
			 memcmp(se->path, newpath, se->len) == 0)
			newprio = i;
		else if (oldlen >= se->len &&
			 memcmp(se->path, oldpath, se->len) == 0)
			oldprio = i;
	}

	if (newprio < 0)
		newprio = bprio;
	if (oldprio < 0)
		oldprio = bprio;

	DBG("priorities: built-in: %d, old: %d, new: %d\n",
	    bprio, newprio, oldprio);

	return newprio <= oldprio;
}

static int depmod_modules_search_file(struct depmod *depmod, size_t baselen, size_t namelen, const char *path)
{
	struct kmod_module *kmod;
	struct mod *mod;
	const char *relpath, *modname;
	const struct ext {
		const char *ext;
		size_t len;
	} *eitr, exts[] = {
		{".ko", sizeof(".ko") - 1},
		{".ko.gz", sizeof(".ko.gz") - 1},
		{NULL, 0},
	};
	size_t modnamelen;
	uint8_t matches = 0;
	int err = 0;

	for (eitr = exts; eitr->ext != NULL; eitr++) {
		if (namelen <= eitr->len)
			continue;
		if (streq(path + baselen + namelen - eitr->len, eitr->ext)) {
			matches = 1;
			break;
		}
	}
	if (!matches)
		return 0;

	relpath = path + depmod->cfg->dirnamelen + 1;
	DBG("try %s\n", relpath);

	err = kmod_module_new_from_path(depmod->ctx, path, &kmod);
	if (err < 0) {
		ERR("Could not create module %s: %s\n",
		    path, strerror(-err));
		return err;
	}

	modname = kmod_module_get_name(kmod);
	mod = hash_find(depmod->modules_by_name, modname);
	if (mod == NULL) {
		err = depmod_module_add(depmod, kmod);
		if (err < 0) {
			ERR("Could not add module %s: %s\n",
			    path, strerror(-err));
			kmod_module_unref(kmod);
			return err;
		}
		return 0;
	}

	modnamelen = strlen(modname);
	if (depmod_module_is_higher_priority(depmod, mod, baselen,
						namelen, modnamelen, path)) {
		DBG("Ignored lower priority: %s, higher: %s\n",
		    path, mod->path);
		kmod_module_unref(kmod);
		return 0;
	}

	err = depmod_module_replace(depmod, mod, kmod);
	if (err < 0) {
		ERR("Could not replace existing module %s\n", path);
		kmod_module_unref(kmod);
		return err;
	}

	return 0;
}

static int depmod_modules_search_dir(struct depmod *depmod, DIR *d, size_t baselen, char *path)
{
	struct dirent *de;
	int err = 0, dfd = dirfd(d);

	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		size_t namelen;
		uint8_t is_dir;

		if (name[0] == '.' && (name[1] == '\0' ||
				       (name[1] == '.' && name[2] == '\0')))
			continue;
		if (streq(name, "build") || streq(name, "source"))
			continue;
		namelen = strlen(name);
		if (baselen + namelen + 2 >= PATH_MAX) {
			path[baselen] = '\0';
			ERR("path is too long %s%s %zd\n", path, name);
			continue;
		}
		memcpy(path + baselen, name, namelen + 1);

		if (de->d_type == DT_REG)
			is_dir = 0;
		else if (de->d_type == DT_DIR)
			is_dir = 1;
		else {
			struct stat st;
			if (fstatat(dfd, name, &st, 0) < 0) {
				ERR("fstatat(%d, %s): %m\n", dfd, name);
				continue;
			} else if (S_ISREG(st.st_mode))
				is_dir = 0;
			else if (S_ISDIR(st.st_mode))
				is_dir = 1;
			else {
				ERR("unsupported file type %s: %o\n",
				    path, st.st_mode & S_IFMT);
				continue;
			}
		}

		if (is_dir) {
			int fd;
			DIR *subdir;
			if (baselen + namelen + 2 + NAME_MAX >= PATH_MAX) {
				ERR("directory path is too long %s\n", path);
				continue;
			}
			fd = openat(dfd, name, O_RDONLY);
			if (fd < 0) {
				ERR("openat(%d, %s, O_RDONLY): %m\n",
				    dfd, name);
				continue;
			}
			subdir = fdopendir(fd);
			if (subdir == NULL) {
				ERR("fdopendir(%d): %m\n", fd);
				close(fd);
				continue;
			}
			path[baselen + namelen] = '/';
			path[baselen + namelen + 1] = '\0';
			err = depmod_modules_search_dir(depmod, subdir,
							baselen + namelen + 1,
							path);
			closedir(subdir);
		} else {
			err = depmod_modules_search_file(depmod, baselen,
							 namelen, path);
		}

		if (err < 0) {
			path[baselen + namelen] = '\0';
			ERR("failed %s: %s\n", path, strerror(-err));
			err = 0; /* ignore errors */
		}
	}

	return err;
}

static int depmod_modules_search(struct depmod *depmod)
{
	char path[PATH_MAX];
	DIR *d = opendir(depmod->cfg->dirname);
	size_t baselen;
	int err;
	if (d == NULL) {
		err = -errno;
		ERR("Couldn't open directory %s: %m\n", depmod->cfg->dirname);
		return err;
	}

	baselen = depmod->cfg->dirnamelen;
	memcpy(path, depmod->cfg->dirname, baselen);
	path[baselen] = '/';
	baselen++;
	path[baselen] = '\0';

	err = depmod_modules_search_dir(depmod, d, baselen, path);
	closedir(d);
	return 0;
}

static int mod_cmp(const void *pa, const void *pb) {
	const struct mod *a = *(const struct mod **)pa;
	const struct mod *b = *(const struct mod **)pb;
	if (a->dep_loop == b->dep_loop)
		return a->sort_idx - b->sort_idx;
	else if (a->dep_loop)
		return 1;
	else if (b->dep_loop)
		return -1;
	return a->sort_idx - b->sort_idx;
}

static void depmod_modules_sort(struct depmod *depmod)
{
	char order_file[PATH_MAX], line[PATH_MAX];
	FILE *fp;
	unsigned idx = 0, total = 0;

	snprintf(order_file, sizeof(order_file), "%s/modules.order",
		 depmod->cfg->dirname);
	fp = fopen(order_file, "r");
	if (fp == NULL) {
		ERR("could not open %s: %m\n", order_file);
		return;
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		idx++;
		if (len == 0)
			continue;
		if (line[len - 1] != '\n') {
			ERR("%s:%u corrupted line misses '\\n'\n",
				order_file, idx);
			goto corrupted;
		}
	}
	total = idx + 1;
	idx = 0;
	fseek(fp, 0, SEEK_SET);
	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		struct mod *mod;

		idx++;
		if (len == 0)
			continue;
		line[len - 1] = '\0';

		mod = hash_find(depmod->modules_by_relpath, line);
		if (mod == NULL)
			continue;
		mod->sort_idx = idx - total;
	}

	array_sort(&depmod->modules, mod_cmp);
	for (idx = 0; idx < depmod->modules.count; idx++) {
		struct mod *m = depmod->modules.array[idx];
		m->idx = idx;
	}

corrupted:
	fclose(fp);
}

static int depmod_symbol_add(struct depmod *depmod, const char *name, uint64_t crc, const struct mod *owner)
{
	size_t namelen;
	int err;
	struct symbol *sym;

	if (name[0] == depmod->cfg->sym_prefix)
		name++;

	namelen = strlen(name) + 1;
	sym = malloc(sizeof(struct symbol) + namelen);
	if (sym == NULL)
		return -ENOMEM;

	sym->owner = (struct mod *)owner;
	sym->crc = crc;
	memcpy(sym->name, name, namelen);

	err = hash_add(depmod->symbols, sym->name, sym);
	if (err < 0) {
		free(sym);
		return err;
	}

	DBG("add %p sym=%s, owner=%p %s\n", sym, sym->name, owner,
	    owner != NULL ? owner->path : "");

	return 0;
}

static struct symbol *depmod_symbol_find(const struct depmod *depmod, const char *name)
{
	if (name[0] == '.') /* PPC64 needs this: .foo == foo */
		name++;
	if (name[0] == depmod->cfg->sym_prefix)
		name++;
	return hash_find(depmod->symbols, name);
}

static int depmod_load_symbols(struct depmod *depmod)
{
	const struct mod **itr, **itr_end;

	DBG("load symbols (%zd modules)\n", depmod->modules.count);

	itr = (const struct mod **)depmod->modules.array;
	itr_end = itr + depmod->modules.count;
	for (; itr < itr_end; itr++) {
		const struct mod *mod = *itr;
		struct kmod_list *l, *list = NULL;
		int err = kmod_module_get_symbols(mod->kmod, &list);
		if (err < 0) {
			DBG("ignoring %s: no symbols: %s\n",
				mod->path, strerror(-err));
			continue;
		}
		kmod_list_foreach(l, list) {
			const char *name = kmod_module_symbol_get_symbol(l);
			uint64_t crc = kmod_module_symbol_get_crc(l);
			depmod_symbol_add(depmod, name, crc, mod);
		}
		kmod_module_symbols_free_list(list);
	}

	DBG("loaded symbols (%zd modules, %zd symbols)\n",
	    depmod->modules.count, hash_get_count(depmod->symbols));

	return 0;
}

static int depmod_load_module_dependencies(struct depmod *depmod, struct mod *mod)
{
	const struct cfg *cfg = depmod->cfg;
	struct kmod_list *l, *list = NULL;
	int err = kmod_module_get_dependency_symbols(mod->kmod, &list);
	if (err < 0) {
		DBG("ignoring %s: no dependency symbols: %s\n",
		    mod->path, strerror(-err));
		return 0;
	}

	DBG("do dependencies of %s\n", mod->path);
	kmod_list_foreach(l, list) {
		const char *name = kmod_module_dependency_symbol_get_symbol(l);
		uint64_t crc = kmod_module_dependency_symbol_get_crc(l);
		int bind = kmod_module_dependency_symbol_get_bind(l);
		struct symbol *sym = depmod_symbol_find(depmod, name);
		uint8_t is_weak = bind == KMOD_SYMBOL_WEAK;

		if (sym == NULL) {
			DBG("%s needs (%c) unknown symbol %s\n",
			    mod->path, bind, name);
			if (cfg->print_unknown && !is_weak)
				WRN("%s needs unknown symbol %s\n",
				    mod->path, name);
			continue;
		}

		if (cfg->check_symvers && sym->crc != crc && !is_weak) {
			DBG("symbol %s (%#"PRIx64") module %s (%#"PRIx64")\n",
			    sym->name, sym->crc, mod->path, crc);
			if (cfg->print_unknown)
				WRN("%s disagrees about version of symbol %s\n",
				    mod->path, name);
		}

		mod_add_dependency(mod, sym);
	}
	kmod_module_dependency_symbols_free_list(list);
	return 0;
}

static int depmod_load_dependencies(struct depmod *depmod)
{
	struct mod **itr, **itr_end;

	DBG("load dependencies (%zd modules, %zd symbols)\n",
	    depmod->modules.count, hash_get_count(depmod->symbols));

	itr = (struct mod **)depmod->modules.array;
	itr_end = itr + depmod->modules.count;
	for (; itr < itr_end; itr++) {
		struct mod *mod = *itr;
		depmod_load_module_dependencies(depmod, mod);
	}

	DBG("loaded dependencies (%zd modules, %zd symbols)\n",
	    depmod->modules.count, hash_get_count(depmod->symbols));

	return 0;
}

static int dep_cmp(const void *pa, const void *pb)
{
	const struct mod *a = *(const struct mod **)pa;
	const struct mod *b = *(const struct mod **)pb;
	if (a->dep_loop == b->dep_loop)
		return a->dep_sort_idx - b->dep_sort_idx;
	else if (a->dep_loop)
		return 1;
	else if (b->dep_loop)
		return -1;
	return a->dep_sort_idx - b->dep_sort_idx;
}

static void depmod_sort_dependencies(struct depmod *depmod)
{
	struct mod **itr, **itr_end;
	itr = (struct mod **)depmod->modules.array;
	itr_end = itr + depmod->modules.count;
	for (; itr < itr_end; itr++) {
		struct mod *m = *itr;
		if (m->deps.count > 1)
			array_sort(&m->deps, dep_cmp);
	}
}

static int depmod_calculate_dependencies(struct depmod *depmod)
{
	const struct mod **itrm;
	uint16_t *users, *roots, *sorted;
	uint16_t i, n_roots = 0, n_sorted = 0, n_mods = depmod->modules.count;

	users = malloc(sizeof(uint16_t) * n_mods * 3);
	if (users == NULL)
		return -ENOMEM;
	roots = users + n_mods;
	sorted = roots + n_mods;

	DBG("calculate dependencies and ordering (%zd modules)\n", n_mods);

	assert(depmod->modules.count < UINT16_MAX);

	/* populate modules users (how many modules uses it) */
	itrm = (const struct mod **)depmod->modules.array;
	for (i = 0; i < n_mods; i++, itrm++) {
		const struct mod *m = *itrm;
		users[i] = m->users;
		if (users[i] == 0) {
			roots[n_roots] = i;
			n_roots++;
		}
	}

	/* topological sort (outputs modules without users first) */
	while (n_roots > 0) {
		const struct mod **itr_dst, **itr_dst_end;
		struct mod *src;
		uint16_t src_idx = roots[--n_roots];

		src = depmod->modules.array[src_idx];
		src->dep_sort_idx = n_sorted;
		sorted[n_sorted] = src_idx;
		n_sorted++;

		itr_dst = (const struct mod **)src->deps.array;
		itr_dst_end = itr_dst + src->deps.count;
		for (; itr_dst < itr_dst_end; itr_dst++) {
			const struct mod *dst = *itr_dst;
			uint16_t dst_idx = dst->idx;
			assert(users[dst_idx] > 0);
			users[dst_idx]--;
			if (users[dst_idx] == 0) {
				roots[n_roots] = dst_idx;
				n_roots++;
			}
		}
	}

	if (n_sorted < n_mods) {
		WRN("found %hu modules in dependency cycles!\n",
		    n_mods - n_sorted);
		for (i = 0; i < n_mods; i++) {
			struct mod *m;
			if (users[i] == 0)
				continue;
			m = depmod->modules.array[i];
			WRN("%s in dependency cycle!\n", m->path);
			m->dep_loop = 1;
			m->dep_sort_idx = INT32_MAX;
			depmod->dep_loops++;
		}
	}

	depmod_sort_dependencies(depmod);

	DBG("calculated dependencies and ordering (%u loops, %zd modules)\n",
	    depmod->dep_loops, n_mods);

	free(users);
	return 0;
}

static int depmod_load(struct depmod *depmod)
{
	int err;

	err = depmod_load_symbols(depmod);
	if (err < 0)
		return err;

	err = depmod_load_dependencies(depmod);
	if (err < 0)
		return err;

	err = depmod_calculate_dependencies(depmod);
	if (err < 0)
		return err;

	return 0;
}

static int output_symbols(struct depmod *depmod, FILE *out)
{
	size_t i;

	fputs("# Aliases for symbols, used by symbol_request().\n", out);

	for (i = 0; i < depmod->symbols->n_buckets; i++) {
		const struct hash_bucket *b = depmod->symbols->buckets + i;
		unsigned j;
		for (j = 0; j < b->used; j++) {
			const struct hash_entry *e = b->entries + j;
			const struct symbol *sym = e->value;
			if (sym->owner == NULL)
				continue;
			fprintf(out, "alias symbol:%s %s\n",
				sym->name, sym->owner->modname);
		}
	}

	return 0;
}

static int output_devname(struct depmod *depmod, FILE *out)
{
	size_t i;

	fputs("# Device nodes to trigger on-demand module loading.\n", out);

	for (i = 0; i < depmod->modules.count; i++) {
		const struct mod *mod = depmod->modules.array[i];
		struct kmod_list *l, *list = NULL;
		const char *devname = NULL;
		char type = '\0';
		unsigned int major = 0, minor = 0;
		int r;

		r = kmod_module_get_info(mod->kmod, &list);
		if (r < 0 || list == NULL)
			continue;

		kmod_list_foreach(l, list) {
			const char *key = kmod_module_info_get_key(l);
			const char *value = kmod_module_info_get_value(l);
			unsigned int maj, min;

			if (!streq(key, "alias"))
				continue;

			if (strstartswith(value, "devname:"))
				devname = value + sizeof("devname:") - 1;
			else if (sscanf(value, "char-major-%u-%u",
						&maj, &min) == 2) {
				type = 'c';
				major = maj;
				minor = min;
			} else if (sscanf(value, "block-major-%u-%u",
						&maj, &min) == 2) {
				type = 'b';
				major = maj;
				minor = min;
			}

			if (type != '\0' && devname != NULL) {
				fprintf(out, "%s %s %c%u:%u\n",
					kmod_module_get_name(mod->kmod),
					devname, type, major, minor);
				break;
			}
		}
		kmod_module_info_free_list(list);
	}

	return 0;
}

static int depmod_output(struct depmod *depmod, FILE *out)
{
	static const struct depfile {
		const char *name;
		int (*cb)(struct depmod *depmod, FILE *out);
	} *itr, depfiles[] = {
		//{"modules.dep", output_deps},
		//{"modules.dep.bin", output_deps_bin},
		//{"modules.alias", output_aliases},
		//{"modules.alias.bin", output_aliases_bin},
		//{"modules.softdep", output_softdeps},
		{"modules.symbols", output_symbols},
		//{"modules.symbols.bin", output_symbols_bin},
		//{"modules.builtin.bin", output_builtin_bin},
		{"modules.devname", output_devname},
		{NULL, NULL}
	};
	const char *dname = depmod->cfg->dirname;
	int dfd, err = 0;

	if (out != NULL)
		dfd = -1;
	else {
		dfd = open(dname, O_RDONLY);
		if (dfd < 0) {
			err = -errno;
			CRIT("Could not open directory %s: %m\n", dname);
			return err;
		}
	}

	for (itr = depfiles; itr->name != NULL; itr++) {
		FILE *fp = out;
		char tmp[NAME_MAX] = "";
		int r;

		if (fp == NULL) {
			int flags = O_CREAT | O_TRUNC | O_WRONLY;
			int mode = 0644;
			int fd;

			snprintf(tmp, sizeof(tmp), "%s.tmp", itr->name);
			fd = openat(dfd, tmp, flags, mode);
			if (fd < 0) {
				ERR("openat(%s, %s, %o, %o): %m\n",
				    dname, tmp, flags, mode);
				continue;
			}
			fp = fdopen(fd, "wb");
			if (fp == NULL) {
				ERR("fdopen(%d=%s/%s): %m\n", fd, dname, tmp);
				close(fd);
				continue;
			}
		}

		r = itr->cb(depmod, fp);
		if (fp == out)
			continue;

		fclose(fp);
		if (r < 0) {
			if (unlinkat(dfd, tmp, 0) != 0)
				ERR("unlinkat(%s, %s): %m\n", dname, tmp);
		} else {
			unlinkat(dfd, itr->name, 0);
			if (renameat(dfd, tmp, dfd, itr->name) != 0) {
				err = -errno;
				CRIT("renameat(%s, %s, %s, %s): %m\n",
				     dname, tmp, dname, itr->name);
				break;
			}
		}
	}

	if (dfd >= 0)
		close(dfd);
	return err;
}


static int depfile_up_to_date(const char *dirname)
{
	ERR("TODO depfile_up_to_date()\n");
	return 0;
}

static int is_version_number(const char *version)
{
	unsigned int d1, d2;
	return (sscanf(version, "%u.%u", &d1, &d2) == 2);
}

int main(int argc, char *argv[])
{
	FILE *out = NULL;
	int i, err = 0, all = 0, maybe_all = 0, n_config_paths = 0;
	const char **config_paths = NULL;
	const char *root = "";
	const char *system_map = NULL;
	const char *module_symvers = NULL;
	const char *null_kmod_config = NULL;
	struct utsname un;
	struct kmod_ctx *ctx = NULL;
	struct cfg cfg;
	struct depmod depmod;

	memset(&cfg, 0, sizeof(cfg));
	memset(&depmod, 0, sizeof(depmod));

	for (;;) {
		int c, idx = 0;
		c = getopt_long(argc, argv, cmdopts_s, cmdopts, &idx);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
			all = 1;
			break;
		case 'A':
			maybe_all = 1;
			break;
		case 'b':
			root = optarg;
			break;
		case 'C': {
			size_t bytes = sizeof(char *) * (n_config_paths + 2);
			void *tmp = realloc(config_paths, bytes);
			if (!tmp) {
				fputs("Error: out-of-memory\n", stderr);
				goto cmdline_failed;
			}
			config_paths = tmp;
			config_paths[n_config_paths] = optarg;
			n_config_paths++;
			config_paths[n_config_paths] = NULL;
			break;
		}
		case 'E':
			module_symvers = optarg;
			cfg.check_symvers = 1;
			break;
		case 'F':
			system_map = optarg;
			break;
		case 'e':
			cfg.print_unknown = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'n':
			out = stdout;
			break;
		case 'P':
			if (optarg[1] != '\0') {
				CRIT("-P only takes a single char\n");
				goto cmdline_failed;
			}
			cfg.sym_prefix = optarg[0];
			break;
		case 'w':
			cfg.warn_dups = 1;
			break;
		case 'u':
		case 'q':
		case 'r':
		case 'm':
			if (idx > 0) {
				fprintf(stderr,
					"ignored deprecated option --%s\n",
					cmdopts[idx].name);
			} else {
				fprintf(stderr,
					"ignored deprecated option -%c\n", c);
			}
			break;
		case 'h':
			help(argv[0]);
			free(config_paths);
			return EXIT_SUCCESS;
		case 'V':
			puts(PACKAGE " version " VERSION);
			free(config_paths);
			return EXIT_SUCCESS;
		case '?':
			goto cmdline_failed;
		default:
			fprintf(stderr,
				"Error: unexpected getopt_long() value '%c'.\n",
				c);
			goto cmdline_failed;
		}
	}

	if (optind < argc && is_version_number(argv[optind])) {
		cfg.kversion = argv[optind];
		optind++;
	} else {
		if (uname(&un) < 0) {
			CRIT("uname() failed: %s\n", strerror(errno));
			goto cmdline_failed;
		}
		cfg.kversion = un.release;
	}

	cfg.dirnamelen = snprintf(cfg.dirname, PATH_MAX,
				  "%s" ROOTPREFIX "/lib/modules/%s",
				  root, cfg.kversion);

	if (optind == argc)
		all = 1;

	if (maybe_all) {
		if (out == stdout)
			goto done;
		if (depfile_up_to_date(cfg.dirname))
			goto done;
		all = 1;
	}

	ctx = kmod_new(cfg.dirname, &null_kmod_config);
	if (ctx == NULL) {
		CRIT("kmod_new(\"%s\", {NULL}) failed: %m\n", cfg.dirname);
		goto cmdline_failed;
	}
	kmod_set_log_priority(ctx, verbose);

	err = depmod_init(&depmod, &cfg, ctx);
	if (err < 0) {
		CRIT("depmod_init: %s\n", strerror(-err));
		goto depmod_init_failed;
	}
	ctx = NULL; /* owned by depmod */

	if (all) {
		err = cfg_load(&cfg, config_paths);
		if (err < 0) {
			CRIT("Could not load configuration files\n");
			goto cmdline_modules_failed;
		}
		err = depmod_modules_search(&depmod);
		if (err < 0) {
			CRIT("Could search modules: %s\n", strerror(-err));
			goto cmdline_modules_failed;
		}
	} else {
		for (i = optind; i < argc; i++) {
			const char *path = argv[i];
			struct kmod_module *mod;

			if (path[0] != '/') {
				CRIT("%s: not absolute path.", path);
				goto cmdline_modules_failed;
			}

			err = kmod_module_new_from_path(depmod.ctx, path, &mod);
			if (err < 0) {
				CRIT("Could not create module %s: %s\n",
				     path, strerror(-err));
				goto cmdline_modules_failed;
			}

			err = depmod_module_add(&depmod, mod);
			if (err < 0) {
				CRIT("Could not add module %s: %s\n",
				     path, strerror(-err));
				kmod_module_unref(mod);
				goto cmdline_modules_failed;
			}
		}
	}

	depmod_modules_sort(&depmod);
	err = depmod_load(&depmod);
	if (err < 0)
		goto cmdline_modules_failed;

	err = depmod_output(&depmod, out);

done:
	depmod_shutdown(&depmod);
	cfg_free(&cfg);
	free(config_paths);
	return err >= 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cmdline_modules_failed:
	depmod_shutdown(&depmod);
depmod_init_failed:
	if (ctx != NULL)
		kmod_unref(ctx);
cmdline_failed:
	cfg_free(&cfg);
	free(config_paths);
	return EXIT_FAILURE;
}