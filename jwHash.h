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

//#define HASHTHREADED  1
//#define HASHTEST      1
//#define HASHDEBUG     1

// guards! guards!
#ifndef jwhash_h
#define jwhash_h

// needed for size_t
#include <stddef.h>

#ifdef HASHDEBUG
# define HASH_DEBUG(fmt,args...) printf(fmt, ## args)
#else
# define HASH_DEBUG(fmt,args...) do {} while (0);
#endif


// resuts codes
typedef enum 
{
	HASHOK,
	HASHADDED,
	HASHREPLACEDVALUE,
	HASHALREADYADDED,
	HASHDELETED,
	HASHNOTFOUND,
} HASHRESULT;

typedef enum
{
	HASHPTR,
	HASHNUMERIC,
	HASHSTRING,
} HASHVALTAG;
	

typedef struct jwHashEntry jwHashEntry;
struct jwHashEntry
{
	union
	{
		char  *strValue;
		double dblValue;
		int	   intValue;
	} key;
	HASHVALTAG valtag;
	union
	{
		char  *strValue;
		double dblValue;
		int	   intValue;
		void  *ptrValue;
	} value;
	jwHashEntry *next;
};

typedef struct jwHashTable jwHashTable;
struct jwHashTable
{
	jwHashEntry **bucket;			// pointer to array of buckets
	size_t buckets;
	size_t bucketsinitial;			// if we resize, may need to hash multiple times
	HASHRESULT lastError;
#ifdef HASHTHREADED
	volatile int *locks;			// array of locks
	volatile int lock;				// lock for entire table
#endif
};

typedef void (*hashtable_free_item_callback)(void *value);

// Create/delete hash table
jwHashTable *create_hash( size_t buckets );
void *delete_hash( jwHashTable *table,  hashtable_free_item_callback free_cb, HASHVALTAG ktype, HASHVALTAG vtype);		// clean up all memory

typedef void (*process_hash_value_callback)(void *value);


//<<< liudengfeng added here
HASHRESULT hash_iterator(jwHashTable *table, process_hash_value_callback process_cb, HASHVALTAG type);
HASHRESULT get_ptr_by_str(jwHashTable *table, char *key, void **ptr);
//>>>

// Add to table - keyed by string
HASHRESULT add_str_by_str( jwHashTable*, char *key, char *value );
HASHRESULT add_dbl_by_str( jwHashTable*, char *key, double value );
HASHRESULT add_int_by_str( jwHashTable*, char *key, long int value );
HASHRESULT add_ptr_by_str( jwHashTable*, char *key, void *value );

// Delete by string
HASHRESULT del_by_str( jwHashTable*, char *key );

// Get by string
HASHRESULT get_str_by_str( jwHashTable *table, char *key, char **value );
HASHRESULT get_int_by_str( jwHashTable *table, char *key, int *i );
HASHRESULT get_dbl_by_str( jwHashTable *table, char *key, double *val );


// Add to table - keyed by int
HASHRESULT add_str_by_int( jwHashTable*, long int key, char *value );
HASHRESULT add_dbl_by_int( jwHashTable*, long int key, double value );
HASHRESULT add_int_by_int( jwHashTable*, long int key, long int value );
HASHRESULT add_ptr_by_int( jwHashTable*, long int key, void *value );

// Delete by int
HASHRESULT del_by_int( jwHashTable*, long int key );

// Get by int
HASHRESULT get_str_by_int( jwHashTable *table, long int key, char **value );
HASHRESULT get_int_by_int( jwHashTable *table, long int key, int *i );
HASHRESULT get_dbl_by_int( jwHashTable *table, long int key, double *val );

#endif



