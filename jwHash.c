/*

Copyright 2015 Jonathan Watmough
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
	http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jwHash.h"

#ifdef HASHTEST
#include <sys/time.h>
#endif

#ifdef HASHTHREADED
#include <pthread.h>
#include <semaphore.h>
#endif

////////////////////////////////////////////////////////////////////////////////
// STATIC HELPER FUNCTIONS

// Spin-locking
// http://stackoverflow.com/questions/1383363/is-my-spin-lock-implementation-correct-and-optimal

// http://stackoverflow.com/a/12996028
// hash function for int keys
static inline long int hashInt(long int x)
{
	x = ((x >> 16) ^ x) * 0x45d9f3b;
	x = ((x >> 16) ^ x) * 0x45d9f3b;
	x = ((x >> 16) ^ x);
	return x;
}

// http://www.cse.yorku.ca/~oz/hash.html
// hash function for string keys djb2
static inline long int hashString(char * str)
{
	unsigned long hash = 5381;
	int c;

	while (c = *str++)
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
	return hash;
}

// helper for copying string keys and values
static inline char * copystring(char * value)
{
	char * copy = (char *)malloc(strlen(value)+1);
	if(!copy) {
		printf("Unable to allocate string value %s\n",value);
		abort();
	}
	strcpy(copy,value);
	return copy;
}


////////////////////////////////////////////////////////////////////////////////
// CREATING A NEW HASH TABLE

// Create hash table
jwHashTable *create_hash( size_t buckets )
{
	// allocate space
	jwHashTable *table= (jwHashTable *)malloc(sizeof(jwHashTable));
	if(!table) {
		// unable to allocate
		return NULL;
	}
	// locks
#ifdef HASHTHREADED
	table->lock = 0;
	table->locks = (int *)malloc(buckets * sizeof(int));
	if( !table->locks ) {
		free(table);
		return NULL;
	}
	memset((int *)&table->locks[0],0,buckets*sizeof(int));
#endif
	// setup
	table->bucket = (jwHashEntry **)malloc(buckets*sizeof(void*));
	if( !table->bucket ) {
		free(table);
		return NULL;
	}
	memset(table->bucket,0,buckets*sizeof(void*));
	table->buckets = table->bucketsinitial = buckets;
	HASH_DEBUG("table: %x bucket: %x\n",table,table->bucket);
	return table;
}

void *delete_hash( jwHashTable *table,  hashtable_free_item_callback free_cb, HASHVALTAG ktype, HASHVALTAG vtype)
{
	int i = 0; 
	for(; i < table->buckets; i++) {
		jwHashEntry *entry = table->bucket[i];
		while(entry) {
			switch(vtype) {
			case HASHPTR:
				free_cb(entry->value.ptrValue);
				break;
			case HASHSTRING:
				free_cb(entry->value.strValue);
				break;
			}
			
			switch(ktype) {
				case HASHSTRING:
					free(entry->key.strValue);
					break;
			}
			jwHashEntry *next = entry->next;
			free(entry);
			entry = next;
		}
	}
	
	free(table->buckets);
}

////////////////////////////////////////////////////////////////////////////////
// ADDING / DELETING / GETTING BY STRING KEY

// Add str to table - keyed by string
HASHRESULT add_str_by_str( jwHashTable *table, char *key, char *value )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("adding %s -> %s hash: %ld\n",key,value,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key) && 0==strcmp(value,entry->value.strValue))
			return HASHALREADYADDED;
		// check for replacing entry
		if(0==strcmp(entry->key.strValue,key) && 0!=strcmp(value,entry->value.strValue))
		{
			free(entry->value.strValue);
			entry->value.strValue = copystring(value);
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.strValue = copystring(key);
	entry->valtag = HASHSTRING;
	entry->value.strValue = copystring(value);
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}

HASHRESULT add_dbl_by_str( jwHashTable *table, char *key, double value )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("adding %s -> %f hash: %ld\n",key,value,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key) && value==entry->value.dblValue)
			return HASHALREADYADDED;
		// check for replacing entry
		if(0==strcmp(entry->key.strValue,key) && value!=entry->value.dblValue)
		{
			entry->value.dblValue = value;
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.strValue = copystring(key);
	entry->valtag = HASHNUMERIC;
	entry->value.dblValue = value;
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}

HASHRESULT add_int_by_str( jwHashTable *table, char *key, long int value )
{
	// compute hash on key
	size_t hash = hashString(key);
	hash %= table->buckets;
	HASH_DEBUG("adding %s -> %d hash: %ld\n",key,value,hash);

#ifdef HASHTHREADED
	// lock this bucket against changes
	while (__sync_lock_test_and_set(&table->locks[hash], 1)) {
		printf(".");
		// Do nothing. This GCC builtin instruction
		// ensures memory barrier.
	  }
#endif

	// check entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key) && value==entry->value.intValue)
			return HASHALREADYADDED;
		// check for replacing entry
		if(0==strcmp(entry->key.strValue,key) && value!=entry->value.intValue)
		{
			entry->value.intValue = value;
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.strValue = copystring(key);
	entry->valtag = HASHNUMERIC;
	entry->value.intValue = value;
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
unlock:
#ifdef HASHTHREADED
	__sync_synchronize(); // memory barrier
	table->locks[hash] = 0;
#endif
	return HASHOK;
}


HASHRESULT hash_iterator(jwHashTable *table, process_hash_value_callback process_cb, HASHVALTAG type)
{
	int i = 0; 
	for(; i < table->buckets; i++) {
		jwHashEntry *entry = table->bucket[i];
		while(entry) {
			switch(type) {
			case HASHPTR:
				process_cb(entry->value.ptrValue);
				break;
			case HASHNUMERIC:
				process_cb(&entry->value.intValue);
				break;
			case HASHSTRING:
				process_cb(entry->value.strValue);
				break;
			}
			
			// move to next entry
			entry = entry->next;
		}
	}
	
	return HASHOK;
}

HASHRESULT get_ptr_by_str(jwHashTable *table, char *key, void **ptr)
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("adding %s -> %x hash: %ld\n",key,ptr,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key) && ptr==entry->value.ptrValue) {
			*ptr = entry->value.ptrValue;
			return HASHOK;
		}
		// move to next entry
		entry = entry->next;
	}
	
	return HASHNOTFOUND;
}

HASHRESULT add_ptr_by_str( jwHashTable *table, char *key, void *ptr )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("adding %s -> %x hash: %ld\n",key,ptr,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key) && ptr==entry->value.ptrValue)
			return HASHALREADYADDED;
		// check for replacing entry
		if(0==strcmp(entry->key.strValue,key) && ptr!=entry->value.ptrValue)
		{
			entry->value.ptrValue = ptr;
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.strValue = copystring(key);
	entry->valtag = HASHPTR;
	entry->value.ptrValue = ptr;
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}

// Delete by string
HASHRESULT del_by_str( jwHashTable *table, char *key )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("deleting: %s hash: %ld\n",key,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	jwHashEntry *previous = NULL;
	
	// found an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(0==strcmp(entry->key.strValue,key))
		{
			// skip first record, or one in the chain
			if(!previous)
				table->bucket[hash] = entry->next;
			else
				previous->next = entry->next;
			// delete string value if needed
			if( entry->valtag==HASHSTRING )
				free(entry->value.strValue);
			free(entry->key.strValue);
			free(entry);
			return HASHDELETED;
		}
		// move to next entry
		previous = entry;
		entry = entry->next;
	}
	return HASHNOTFOUND;
}

// Lookup str - keyed by str
HASHRESULT get_str_by_str( jwHashTable *table, char *key, char **value )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("fetching %s -> ?? hash: %d\n",key,hash);

	// get entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	while(entry)
	{
		// check for key
		HASH_DEBUG("found entry key: %d value: %s\n",entry->key.intValue,entry->value.strValue);
		if(0==strcmp(entry->key.strValue,key)) {
			*value =  entry->value.strValue;
			return HASHOK;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// not found
	return HASHNOTFOUND;
}

// Lookup int - keyed by str
HASHRESULT get_int_by_str( jwHashTable *table, char *key, int *i )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("fetching %s -> ?? hash: %d\n",key,hash);

	// get entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	while(entry)
	{
		// check for key
		HASH_DEBUG("found entry key: %s value: %ld\n",entry->key.strValue,entry->value.intValue);
		if(0==strcmp(entry->key.strValue,key)) {
			*i = entry->value.intValue;
			return HASHOK;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// not found
	return HASHNOTFOUND;
}

// Lookup dbl - keyed by str
HASHRESULT get_dbl_by_str( jwHashTable *table, char *key, double *val )
{
	// compute hash on key
	size_t hash = hashString(key) % table->buckets;
	HASH_DEBUG("fetching %s -> ?? hash: %d\n",key,hash);

	// get entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	while(entry)
	{
		// check for key
		HASH_DEBUG("found entry key: %s value: %f\n",entry->key.strValue,entry->value.dblValue);
		if(0==strcmp(entry->key.strValue,key)) {
			*val = entry->value.dblValue;
			return HASHOK;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// not found
	return HASHNOTFOUND;
}

////////////////////////////////////////////////////////////////////////////////
// ADDING / DELETING / GETTING BY LONG INT KEY

// Add to table - keyed by int
HASHRESULT add_str_by_int( jwHashTable *table, long int key, char *value )
{
	// compute hash on key
	size_t hash = hashInt(key) % table->buckets;
	HASH_DEBUG("adding %d -> %s hash: %d\n",key,value,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(entry->key.intValue==key && 0==strcmp(value,entry->value.strValue))
			return HASHALREADYADDED;
		// check for replacing entry
		if(entry->key.intValue==key && 0!=strcmp(value,entry->value.strValue))
		{
			free(entry->value.strValue);
			entry->value.strValue = copystring(value);
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.intValue = key;
	entry->valtag = HASHSTRING;
	entry->value.strValue = copystring(value);
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}

// Add dbl to table - keyed by int
HASHRESULT add_dbl_by_int( jwHashTable* table, long int key, double value )
{
	// compute hash on key
	size_t hash = hashInt(key) % table->buckets;
	HASH_DEBUG("adding %d -> %f hash: %d\n",key,value,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(entry->key.intValue==key && value==entry->value.dblValue)
			return HASHALREADYADDED;
		// check for replacing entry
		if(entry->key.intValue==key && value!=entry->value.dblValue)
		{
			entry->value.dblValue = value;
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.intValue = key;
	entry->valtag = HASHNUMERIC;
	entry->value.dblValue = value;
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}

HASHRESULT add_int_by_int( jwHashTable* table, long int key, long int value )
{
	// compute hash on key
	size_t hash = hashInt(key) % table->buckets;
	HASH_DEBUG("adding %d -> %d hash: %d\n",key,value,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for already indexed
		if(entry->key.intValue==key && value==entry->value.intValue)
			return HASHALREADYADDED;
		// check for replacing entry
		if(entry->key.intValue==key && value!=entry->value.intValue)
		{
			entry->value.intValue = value;
			return HASHREPLACEDVALUE;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// create a new entry and add at head of bucket
	HASH_DEBUG("creating new entry\n");
	entry = (jwHashEntry *)malloc(sizeof(jwHashEntry));
	HASH_DEBUG("new entry: %x\n",entry);
	entry->key.intValue = key;
	entry->valtag = HASHNUMERIC;
	entry->value.intValue = value;
	entry->next = table->bucket[hash];
	table->bucket[hash] = entry;
	HASH_DEBUG("added entry\n");
	return HASHOK;
}


// Delete by int
HASHRESULT del_by_int( jwHashTable* table, long int key )
{
	// compute hash on key
	size_t hash = hashInt(key) % table->buckets;
	HASH_DEBUG("deleting: %d hash: %d\n",key,hash);

	// add entry
	jwHashEntry *entry = table->bucket[hash];
	jwHashEntry *prev = NULL;
	
	// found an entry
	HASH_DEBUG("entry: %x\n",entry);
	while(entry!=0)
	{
		HASH_DEBUG("checking entry: %x\n",entry);
		// check for entry to delete
		if(entry->key.intValue==key)
		{
			// skip first record, or one in the chain
			if(!prev)
				table->bucket[hash] = entry->next;
			else
				prev->next = entry->next;
			// delete string value if needed
			if( entry->valtag==HASHSTRING )
				free(entry->value.strValue);
			free(entry);
			return HASHDELETED;
		}
		// move to next entry
		prev = entry;
		entry = entry->next;
	}
	return HASHNOTFOUND;
}

// Lookup str - keyed by int
HASHRESULT get_str_by_int( jwHashTable *table, long int key, char **value )
{
	// compute hash on key
	size_t hash = hashInt(key) % table->buckets;
	HASH_DEBUG("fetching %d -> ?? hash: %d\n",key,hash);

	// get entry
	jwHashEntry *entry = table->bucket[hash];
	
	// already an entry
	while(entry)
	{
		// check for key
		HASH_DEBUG("found entry key: %d value: %s\n",entry->key.intValue,entry->value.strValue);
		if(entry->key.intValue==key) {
			*value = entry->value.strValue;
			return HASHOK;
		}
		// move to next entry
		entry = entry->next;
	}
	
	// not found
	return HASHNOTFOUND;
}






