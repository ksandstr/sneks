
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>


static size_t hash_env_str(const void *ptr, void *priv);


/* members are char **, which allows for both self-allocated and
 * externally-defined environment items.
 */
static struct htable env_ht = HTABLE_INITIALIZER(env_ht, &hash_env_str, NULL);


static size_t hash_env_str(const void *ptr, void *priv)
{
	const char *s = *(const char **)ptr;
	char *end = strchr(s, '=');
	return hash(s, end != NULL ? end - s : strlen(s), (size_t)priv);
}


static bool cmp_env_str(const void *entry, void *key)
{
	const char *s = *(const char **)entry;
	char *end = strchr(s, '=');
	if(end == NULL) return false;
	return strncmp(s, key, end - s) == 0;
}


static inline size_t hash_key(const char *key) {
	return hash(key, strlen(key), (size_t)env_ht.priv);
}


char *getenv(const char *name)
{
	char **ent = htable_get(&env_ht, hash_key(name), &cmp_env_str, name);
	if(ent == NULL) return NULL;
	char *sep = strchr(*ent, '=');
	return sep == NULL ? "" : sep + 1;
}


int setenv(const char *name, const char *value, int overwrite)
{
	if(name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return -1;
	}
	size_t namelen = strlen(name);
	size_t hash = hash_key(name);
	char **old = htable_get(&env_ht, hash, &cmp_env_str, name);
	if(old != NULL) {
		if(!overwrite) return 0;
		else {
			htable_del(&env_ht, hash, old);
			free(old);
			assert(htable_get(&env_ht, hash, &cmp_env_str, name) == NULL);
		}
	}

	size_t valuelen = strlen(value);
	struct {
		char *start;
		char str[];
	} *item = malloc(sizeof *item + namelen + valuelen + 2);
	if(item == NULL) goto Enomem;

	memcpy(item->str, name, namelen);
	item->str[namelen] = '=';
	memcpy(&item->str[namelen + 1], value, valuelen);
	item->str[namelen + 1 + valuelen] = '\0';
	item->start = item->str;
	bool ok = htable_add(&env_ht, hash, &item->start);
	if(!ok) goto Enomem;
	return 0;

Enomem:
	errno = ENOMEM;
	return -1;
}


int unsetenv(const char *name)
{
	if(name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return -1;
	}
	size_t hash = hash_key(name);
	struct htable_iter it;
	for(char **cur = htable_firstval(&env_ht, &it, hash);
		cur != NULL;
		cur = htable_nextval(&env_ht, &it, hash))
	{
		if(cmp_env_str(cur, (void *)name)) {
			htable_delval(&env_ht, &it);
			free(cur);
			assert(htable_get(&env_ht, hash, &cmp_env_str, name) == NULL);
			break;
		}
	}
	return 0;
}


/* NOTE: this lacks quite a bit of functionality, and trying to overwrite
 * things with it will lead to assert breakage in unsetenv and overwriting
 * setenv.
 */
int putenv(char *string)
{
	/* check input validity, separate key from value. */
	if(string == NULL || *string == '\0') return 0;
	char *eq = strchr(string, '=');
	if(eq == NULL || eq == string) return 0;
	char key[eq - string + 1];
	memcpy(key, string, eq - string);
	key[eq - string] = '\0';

	char **header = malloc(sizeof *header);
	if(header == NULL) goto Enomem;
	*header = string;

	size_t hash = hash_env_str(header, env_ht.priv);

	/* check overwrite. */
	struct htable_iter it;
	for(char **cur = htable_firstval(&env_ht, &it, hash);
		cur != NULL;
		cur = htable_nextval(&env_ht, &it, hash))
	{
		if(cmp_env_str(cur, (void *)key)) {
			htable_delval(&env_ht, &it);
			free(cur);
			assert(htable_get(&env_ht, hash, &cmp_env_str, key) == NULL);
			break;
		}
	}

	bool ok = htable_add(&env_ht, hash, header);
	if(!ok) goto Enomem;

	return 0;

Enomem:
	errno = ENOMEM;
	return -1;
}


int clearenv(void)
{
	struct htable_iter it;
	for(char **cur = htable_first(&env_ht, &it);
		cur != NULL;
		cur = htable_next(&env_ht, &it))
	{
		htable_delval(&env_ht, &it);
		free(cur);
	}
	htable_clear(&env_ht);
	htable_init(&env_ht, &hash_env_str, NULL);
	return 0;
}
