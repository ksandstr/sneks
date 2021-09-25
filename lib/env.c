/* ISO C11 getenv(), setenv(), unsetenv(), environ.
 *
 * this topic is laden with historical fuckery, so we try to muddle through as
 * best possible while providing htable key lookup, never free()ing
 * statically-allocated environment values in "environ", and not leaking
 * memory.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>

#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>


struct envstr
{
	size_t hash;
	char *str;
	short keylen;
	bool ours;
};


static size_t hash_envstr(const void *, void *);


char **environ = NULL;

static char **cached_environ = NULL;
static bool our_environ = false;
static struct htable env_ht = HTABLE_INITIALIZER(env_ht, &hash_envstr, NULL);


static size_t hash_envstr(const void *ptr, void *priv) {
	const struct envstr *e = ptr;
	assert(e->hash == hash(e->str, e->keylen, 0));
	return e->hash;
}


static bool cmp_envstr_key(const void *entry, void *key) {
	const struct envstr *e = entry;
	return strncmp(e->str, key, e->keylen) == 0 && ((char *)key)[e->keylen] == '\0';
}


static int cmp_envstr_by_strptr(const void *a, const void *b) {
	const struct envstr *ea = *(const void **)a, *eb = *(const void **)b;
	return (intptr_t)ea->str - (intptr_t)eb->str;
}


/* TODO: this could be more robust against ENOMEM; currently it loses track of
 * which strings were allocated by us, leaking them when they're removed by
 * unsetenv() which tends to make whatever gave rise to ENOMEM worse in
 * programs that heavily lean on the environment facility.
 */
static bool refresh(void)
{
	/* collect pointers to our strings and sort by ->str value */
	struct envstr *ours[htable_count(&env_ht)];
	int n_ours = 0;
	struct htable_iter it;
	for(struct envstr *e = htable_first(&env_ht, &it);
		e != NULL; e = htable_next(&env_ht, &it))
	{
		if(e->ours) ours[n_ours++] = e; else free(e);
	}
	htable_clear(&env_ht);
	qsort(ours, n_ours, sizeof *ours, &cmp_envstr_by_strptr);

	/* rebuild hash table */
	bool ok = true;
	for(int i=0; ok && environ != NULL && environ[i] != NULL; i++) {
		struct envstr **ep = bsearch(
			&(struct envstr *){ &(struct envstr){ .str = environ[i] } },
			ours, n_ours, sizeof *ours, &cmp_envstr_by_strptr);
		if(ep != NULL) {
			ok = htable_add(&env_ht, (*ep)->hash, *ep);
			if(ok) memmove(ep, ep + 1, (--n_ours - (ep - &ours[0])) * sizeof *ep);
		} else {
			struct envstr *ent = malloc(sizeof *ent);
			if(ent == NULL) ok = false;
			else {
				char *env = environ[i], *eq = strchr(env, '=');
				int keylen = eq != NULL ? eq - env : strlen(env);
				assert(keylen <= SHRT_MAX);
				*ent = (struct envstr){
					.hash = hash(env, keylen, 0), .ours = false,
					.str = env, .keylen = keylen,
				};
				ok = htable_add(&env_ht, ent->hash, ent);
			}
		}
	}
	for(int i=0; i < n_ours; i++) {
		if(ok) free(ours[i]->str);
		free(ours[i]);
	}
	if(ok) cached_environ = environ;
	else {
		for(struct envstr *es = htable_first(&env_ht, &it);
			es != NULL; es = htable_next(&env_ht, &it))
		{
			free(es);
		}
		htable_clear(&env_ht);
		cached_environ = NULL;
	}
	return ok;
}


char *getenv(const char *key)
{
	if(cached_environ != environ) refresh();
	if(cached_environ == NULL) {
		/* linear search due to refresh fail */
		for(int i=0; environ != NULL && environ[i] != NULL; i++) {
			char *eq = strchr(environ[i], '=');
			if(eq == NULL) continue;
			if(strncmp(key, environ[i], eq - environ[i]) == 0) return eq + 1;
		}
		return NULL;
	} else {
		/* turbo asshole mode */
		struct envstr *ent = htable_get(&env_ht, hash(key, strlen(key), 0),
			&cmp_envstr_key, key);
		assert(ent == NULL || ent->str[ent->keylen] == '=');
		return ent == NULL ? NULL : ent->str + ent->keylen + 1;
	}
}


int setenv(const char *name, const char *value, int overwrite)
{
	if(name == NULL || *name == '\0' || strchr(name, '=') != NULL) goto Einval;
	size_t namelen = strlen(name), hash = hash(name, namelen, 0);
	if(namelen > SHRT_MAX) goto Einval;
	struct envstr *old = htable_get(&env_ht, hash, &cmp_envstr_key, name);
	if(old != NULL && !overwrite) return 0;

	struct envstr *ent = NULL;
	if(cached_environ != environ && !refresh()) goto Enomem;

	/* construct and insert new entry */
	ent = malloc(sizeof *ent);
	if(ent == NULL) goto Enomem;
	size_t vlen = strlen(value);
	*ent = (struct envstr){
		.keylen = namelen, .str = malloc(namelen + vlen + 2),
		.hash = hash, .ours = true,
	};
	if(ent->str == NULL) goto Enomem;
	memcpy(ent->str, name, namelen);
	ent->str[namelen] = '=';
	memcpy(ent->str + namelen + 1, value, vlen);
	ent->str[namelen + 1 + vlen] = '\0';
	if(ent->str == NULL || !htable_add(&env_ht, hash, ent)) goto Enomem;

	/* insert into environ */
	int env_len = 0;
	while(environ != NULL && environ[env_len] != NULL) env_len++;
	size_t nesiz = (env_len + 2) * sizeof *environ;
	char **new_env = realloc(our_environ ? environ : NULL, nesiz);
	if(new_env == NULL) {
		htable_del(&env_ht, hash, ent);
		goto Enomem;
	}
	if(!our_environ) {
		memcpy(new_env, environ, env_len * sizeof *environ);
		our_environ = true;
	}
	new_env[env_len++] = ent->str;
	new_env[env_len] = NULL;
	cached_environ = environ = new_env;

	/* remove old value if it existed. */
	if(old != NULL) {
		bool found = false;
		for(int i=0; i < env_len - 1; i++) {
			if(environ[i] == old->str) {
				memmove(environ + i, environ + i + 1, (--env_len - i) * sizeof *environ);
				environ[env_len] = NULL;
				found = true;
				break;
			}
		}
		assert(found);
		htable_del(&env_ht, hash, old);
		if(old->ours) free(old->str);
		free(old);
	}

	return 0;

Einval: errno = EINVAL; return -1;
Enomem:
	free(ent->str);
	free(ent);
	errno = ENOMEM;
	return -1;
}


int unsetenv(const char *name)
{
	if(name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return -1;
	}
	if(cached_environ != environ && !refresh()) {
		/* TODO: this violates POSIX; unsetenv() should only fail when @name
		 * is invalid, and otherwise succeed always.
		 */
		abort();
	}

	size_t hash = hash(name, strlen(name), 0);
	struct envstr *ent = htable_get(&env_ht, hash, &cmp_envstr_key, name);
	if(ent != NULL) {
		htable_del(&env_ht, hash, ent);
		char *p = ent->str;
		if(ent->ours) free(ent->str);
		free(ent);

		assert(environ != NULL);
		int o = 0;
		for(int i = 0; environ[i] != NULL; i++) {
			if(environ[i] != p) environ[o++] = environ[i];
		}
		environ[o] = NULL;
	}

	return 0;
}


int putenv(char *string)
{
	if(string == NULL || *string == '\0') return 0;
	char *eq = strchr(string, '=');
	if(eq == NULL || eq == string) return 0;
	int keylen = eq - string;
	char key[keylen + 1];
	if(keylen > SHRT_MAX) goto Enomem;	/* foo */

	if(cached_environ != environ && !refresh()) goto Enomem;
	struct envstr *ent = malloc(sizeof *ent);
	if(ent == NULL) goto Enomem;

	size_t hash = hash(string, keylen, 0);
	*ent = (struct envstr){
		.str = string, .keylen = keylen, .hash = hash, .ours = false,
	};
	memcpy(key, string, keylen); key[keylen] = '\0';
	struct envstr *old = htable_get(&env_ht, hash, &cmp_envstr_key, key);
	if(!htable_add(&env_ht, hash, ent)) {
		free(ent);
		goto Enomem;
	}
	if(old != NULL) {
		assert(environ != NULL);
		bool found = false;
		for(int i=0; environ[i] != NULL; i++) {
			if(environ[i] == old->str) {
				environ[i] = ent->str;
				found = true;
				break;
			}
		}
		assert(found);
		htable_del(&env_ht, hash, old);
		if(old->ours) free(old->str);
		free(old);
	} else {
		int env_len = 0;
		while(environ != NULL && environ[env_len] != NULL) env_len++;
		char **new_env = realloc(our_environ ? environ : NULL,
			(env_len + 2) * sizeof *new_env);
		if(new_env == NULL) {
			htable_del(&env_ht, hash, ent);
			free(ent);
			goto Enomem;
		}
		if(!our_environ) {
			memcpy(new_env, environ, env_len * sizeof *environ);
			our_environ = true;
		}
		cached_environ = environ = new_env;
		environ[env_len++] = ent->str;
		environ[env_len] = NULL;
	}

	return 0;

Enomem: errno = ENOMEM; return -1;
}


int clearenv(void)
{
	if(cached_environ == environ || refresh()) {
		struct htable_iter it;
		for(struct envstr *cur = htable_first(&env_ht, &it);
			cur != NULL; cur = htable_next(&env_ht, &it))
		{
			if(cur->ours) free(cur->str);
			free(cur);
		}
		htable_clear(&env_ht);
	}
	assert(htable_count(&env_ht) == 0);
	if(our_environ) free(environ);
	cached_environ = environ = NULL;
	return 0;
}
