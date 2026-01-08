/**
 * mapreduce.c - MapReduce Library Implementation
 * 
 * This library implements a single-system MapReduce framework with:
 * - Partitioned intermediate storage (P partitions)
 * - Concurrent key-value emission with sorted keys
 * - Iterator-based value retrieval for Reduce phase
 * - DJB2 hash-based partitioning
 * 
 * Architecture:
 * - Map phase: Parallel processing of files
 * - Shuffle: Automatic (keys sorted within partitions)
 * - Reduce phase: Parallel processing of partitions
 */

#include "mapreduce.h"
#include "threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

/**
 * Data Structures for Intermediate Storage
 */

// Single value in a value list
typedef struct Value_node {
    char* value;                    // Dynamically allocated string
    struct Value_node* next;        // Next value for same key
} Value_node;

// Key entry with associated values
typedef struct Key_entry {
    char* key;                      // Dynamically allocated string (sorted)
    Value_node* values;             // Linked list of values for this key
    Value_node* values_tail;        // Tail pointer for O(1) append
    struct Key_entry* next;         // Next key in sorted order
} Key_entry;

// Partition: stores subset of key-value pairs
typedef struct {
    Key_entry* head;                // Sorted linked list of keys
    pthread_mutex_t partition_mutex; // Protects concurrent access
    unsigned int key_count;         // Number of distinct keys
    unsigned long total_pairs;      // Total key-value pairs (for SJF)
} Partition;

// Iterator state for MR_GetNext (one per partition)
typedef struct {
    Key_entry* current_key;         // Current key being iterated
    Value_node* current_value;      // Current position in value list
} IteratorState;

/**
 * Global State
 */
static Partition* partitions = NULL;        // Array of P partitions
static unsigned int num_partitions = 0;     // P (number of partitions)
static ThreadPool_t* global_threadpool = NULL; // Shared thread pool
static IteratorState* iterators = NULL;     // Iterator state per partition

/**
 * Arguments for Map and Reduce tasks
 */
typedef struct {
    char* filename;
    Mapper mapper_func;
} MapArgs;

typedef struct {
    unsigned int partition_idx;
    Reducer reducer_func;
} ReduceArgs;

/**
 * Helper structures for SJF sorting
 */
typedef struct {
    char* filename;
    unsigned long size;
} FileInfo;

typedef struct {
    unsigned int partition_idx;
    unsigned long size;
} PartitionInfo;

/**
 * MR_Partitioner - Hash function to determine partition for a key
 * 
 * @param key: Key string
 * @param num_partitions: Total number of partitions (P)
 * @return: Partition index [0, P-1]
 * 
 * Uses DJB2 hash algorithm:
 * - Fast single-pass computation
 * - Good distribution (avoids clustering)
 * - Deterministic (same key always maps to same partition)
 */
unsigned int MR_Partitioner(char* key, unsigned int num_partitions) {
    unsigned long hash = 5381;
    int c;
    
    // DJB2 hash: hash = hash * 33 + c
    while ((c = *key++) != '\0') {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }
    
    return hash % num_partitions;
}

/**
 * MR_Emit - Emit a key-value pair to intermediate storage
 * 
 * @param key: Key string (will be duplicated)
 * @param value: Value string (will be duplicated)
 * 
 * Process:
 * 1. Determine partition using MR_Partitioner
 * 2. Lock partition mutex
 * 3. Search for key in sorted key list
 * 4. If key exists: append value to value list
 * 5. If key doesn't exist: insert new key entry (maintain sort order)
 * 6. Unlock partition mutex
 * 
 * Synchronization:
 * - Partition-level locking (finer-grained than global lock)
 * - Multiple threads can write to different partitions concurrently
 * - Sorted insertion maintains key ordering
 * 
 * Memory:
 * - Duplicates both key and value (caller may reuse buffers)
 * - Freed during partition cleanup in MR_Run
 */
void MR_Emit(char* key, char* value) {
    // Determine target partition
    unsigned int p = MR_Partitioner(key, num_partitions);
    
    // Duplicate strings (caller may reuse their buffers)
    char* key_copy = strdup(key);
    char* value_copy = strdup(value);
    
    // CRITICAL SECTION: Modify partition
    pthread_mutex_lock(&partitions[p].partition_mutex);
    
    // Search for key in sorted list (using strcmp for lexicographic order)
    Key_entry* prev = NULL;
    Key_entry* curr = partitions[p].head;
    
    while (curr != NULL && strcmp(curr->key, key_copy) < 0) {
        prev = curr;
        curr = curr->next;
    }
    
    // Check if key already exists
    if (curr != NULL && strcmp(curr->key, key_copy) == 0) {
        // KEY EXISTS: Append value to existing key's value list
        free(key_copy);  // Don't need duplicate key
        
        Value_node* new_value = (Value_node*)malloc(sizeof(Value_node));
        new_value->value = value_copy;
        new_value->next = NULL;
        
        // Append to tail for O(1) insertion
        if (curr->values_tail) {
            curr->values_tail->next = new_value;
        } else {
            curr->values = new_value;
        }
        curr->values_tail = new_value;
        
    } else {
        // KEY DOESN'T EXIST: Insert new key entry (maintaining sorted order)
        Key_entry* new_key = (Key_entry*)malloc(sizeof(Key_entry));
        new_key->key = key_copy;
        
        Value_node* new_value = (Value_node*)malloc(sizeof(Value_node));
        new_value->value = value_copy;
        new_value->next = NULL;
        
        new_key->values = new_value;
        new_key->values_tail = new_value;
        new_key->next = curr;
        
        // Insert into sorted list
        if (prev == NULL) {
            partitions[p].head = new_key;
        } else {
            prev->next = new_key;
        }
        
        partitions[p].key_count++;
    }
    
    // Update partition statistics (for SJF in Reduce phase)
    partitions[p].total_pairs++;
    
    pthread_mutex_unlock(&partitions[p].partition_mutex);
    // END CRITICAL SECTION
}

/**
 * MR_GetNext - Get next value for a given key in a partition
 * 
 * @param key: Key to retrieve values for
 * @param partition_idx: Partition index
 * @return: Duplicated value string (CALLER MUST FREE), or NULL if no more values
 * 
 * Iterator Pattern:
 * - First call for a key: searches for key, initializes iterator
 * - Subsequent calls: advances iterator through value list
 * - Returns NULL when no more values
 * 
 * Thread Safety:
 * - No locking needed (only one Reduce task per partition at a time)
 * - Iterator state is per-partition (no conflicts)
 * 
 * Memory:
 * - Returns duplicated value (CALLER MUST FREE)
 * - Original values freed during partition cleanup
 */
char* MR_GetNext(char* key, unsigned int partition_idx) {
    // No lock needed: only one Reduce task per partition at a time
    
    IteratorState* iter = &iterators[partition_idx];
    
    // First call for this key: initialize iterator
    if (iter->current_key == NULL || 
        strcmp(iter->current_key->key, key) != 0) {
        
        // Search for key in partition
        Key_entry* entry = partitions[partition_idx].head;
        while (entry != NULL && strcmp(entry->key, key) != 0) {
            entry = entry->next;
        }
        
        // Initialize iterator
        iter->current_key = entry;
        iter->current_value = (entry != NULL) ? entry->values : NULL;
    }
    
    // Return next value
    if (iter->current_value == NULL) {
        return NULL;  // No more values for this key
    }
    
    // Duplicate value for caller (they must free it)
    char* value = strdup(iter->current_value->value);
    
    // Advance iterator
    iter->current_value = iter->current_value->next;
    
    return value;
}

/**
 * MR_Map - Wrapper function for Map tasks
 * 
 * @param threadarg: MapArgs structure containing filename and mapper function
 * 
 * Process:
 * 1. Extract arguments
 * 2. Call user's Mapper function
 * 3. Free argument structure
 * 
 * Note: User's Mapper calls MR_Emit to write key-value pairs
 */
void MR_Map(void* threadarg) {
    MapArgs* args = (MapArgs*)threadarg;
    
    // Call user's Map function
    // User's Map function will call MR_Emit to emit key-value pairs
    args->mapper_func(args->filename);
    
    // Free argument structure
    free(args);
}

/**
 * MR_Reduce - Wrapper function for Reduce tasks
 * 
 * @param threadarg: ReduceArgs structure containing partition index and reducer function
 * 
 * Process:
 * 1. Extract arguments
 * 2. Iterate through all keys in partition
 * 3. For each key, call user's Reducer function
 * 4. Free argument structure
 * 
 * Note: User's Reducer calls MR_GetNext to iterate through values
 */
void MR_Reduce(void* threadarg) {
    ReduceArgs* args = (ReduceArgs*)threadarg;
    unsigned int p = args->partition_idx;
    Reducer reducer = args->reducer_func;
    
    // Iterate through all keys in partition
    Key_entry* key_entry = partitions[p].head;
    while (key_entry != NULL) {
        // Reset iterator for this key
        iterators[p].current_key = NULL;
        iterators[p].current_value = NULL;
        
        // Call user's Reduce function
        // User's Reducer will call MR_GetNext to iterate through values
        reducer(key_entry->key, p);
        
        key_entry = key_entry->next;
    }
    
    // Free argument structure
    free(args);
}

/**
 * Comparator functions for SJF sorting
 */
static int compare_file_size(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    if (fa->size < fb->size) return -1;
    if (fa->size > fb->size) return 1;
    return 0;
}

static int compare_partition_size(const void* a, const void* b) {
    PartitionInfo* pa = (PartitionInfo*)a;
    PartitionInfo* pb = (PartitionInfo*)b;
    
    if (pa->size < pb->size) return -1;
    if (pa->size > pb->size) return 1;
    return 0;
}

/**
 * MR_Run - Main MapReduce orchestrator
 * 
 * @param file_count: Number of input files
 * @param file_names: Array of filename strings
 * @param mapper: User's Map function
 * @param reducer: User's Reduce function
 * @param num_workers: Number of threads in thread pool
 * @param num_parts: Number of partitions (P)
 * 
 * Execution Flow:
 * 1. Initialize partitions and thread pool
 * 2. MAP PHASE: Sort files by size, submit Map jobs (SJF)
 * 3. BARRIER: ThreadPool_check() waits for all Map jobs
 * 4. REDUCE PHASE: Sort partitions by size, submit Reduce jobs (SJF)
 * 5. BARRIER: ThreadPool_check() waits for all Reduce jobs
 * 6. Cleanup: Free all data structures
 * 
 * Synchronization:
 * - ThreadPool_check() provides barrier between Map and Reduce
 * - Partition mutexes protect concurrent MR_Emit calls
 * - Same thread pool used for both phases (reuse)
 * 
 * Memory Management:
 * - All dynamically allocated memory freed before return
 * - Traverses all partitions → keys → values
 * - Valgrind clean (no leaks)
 */
void MR_Run(unsigned int file_count, char* file_names[],
            Mapper mapper, Reducer reducer,
            unsigned int num_workers, unsigned int num_parts) {
    
    num_partitions = num_parts;
    
    // ===== STEP 1: Initialize Partitions =====
    partitions = (Partition*)calloc(num_parts, sizeof(Partition));
    iterators = (IteratorState*)calloc(num_parts, sizeof(IteratorState));
    
    for (unsigned int i = 0; i < num_parts; i++) {
        partitions[i].head = NULL;
        partitions[i].key_count = 0;
        partitions[i].total_pairs = 0;
        pthread_mutex_init(&partitions[i].partition_mutex, NULL);
        
        iterators[i].current_key = NULL;
        iterators[i].current_value = NULL;
    }
    
    // ===== STEP 2: Create Thread Pool =====
    global_threadpool = ThreadPool_create(num_workers);
    
    // ===== STEP 3: MAP PHASE with SJF =====
    
    // 3a. Get file sizes for SJF scheduling
    FileInfo* files = (FileInfo*)malloc(file_count * sizeof(FileInfo));
    for (unsigned int i = 0; i < file_count; i++) {
        files[i].filename = file_names[i];
        
        // Get file size using stat()
        struct stat st;
        if (stat(file_names[i], &st) == 0) {
            files[i].size = st.st_size;
        } else {
            files[i].size = 0;  // Error: treat as empty file
        }
    }
    
    // 3b. Sort files by size (ascending for SJF)
    qsort(files, file_count, sizeof(FileInfo), compare_file_size);
    
    // 3c. Submit Map jobs to thread pool (in SJF order)
    for (unsigned int i = 0; i < file_count; i++) {
        MapArgs* args = (MapArgs*)malloc(sizeof(MapArgs));
        args->filename = files[i].filename;
        args->mapper_func = mapper;
        
        ThreadPool_add_job(global_threadpool, 
                          (thread_func_t)MR_Map,
                          args,
                          files[i].size);  // SJF: job size = file size
    }
    
    free(files);
    
    // 3d. BARRIER: Wait for all Map jobs to complete
    ThreadPool_check(global_threadpool);
    
    // ===== STEP 4: REDUCE PHASE with SJF =====
    
    // 4a. Get partition sizes for SJF scheduling
    PartitionInfo* parts = (PartitionInfo*)malloc(num_parts * sizeof(PartitionInfo));
    for (unsigned int i = 0; i < num_parts; i++) {
        parts[i].partition_idx = i;
        parts[i].size = partitions[i].total_pairs;  // SJF: job size = # key-value pairs
    }
    
    // 4b. Sort partitions by size (ascending for SJF)
    qsort(parts, num_parts, sizeof(PartitionInfo), compare_partition_size);
    
    // 4c. Submit Reduce jobs to thread pool (in SJF order)
    for (unsigned int i = 0; i < num_parts; i++) {
        ReduceArgs* args = (ReduceArgs*)malloc(sizeof(ReduceArgs));
        args->partition_idx = parts[i].partition_idx;
        args->reducer_func = reducer;
        
        ThreadPool_add_job(global_threadpool,
                          (thread_func_t)MR_Reduce,
                          args,
                          parts[i].size);  // SJF: job size = partition size
    }
    
    free(parts);
    
    // 4d. BARRIER: Wait for all Reduce jobs to complete
    ThreadPool_check(global_threadpool);
    
    // ===== STEP 5: Cleanup =====
    
    // Destroy thread pool
    ThreadPool_destroy(global_threadpool);
    global_threadpool = NULL;
    
    // Free partition data structures
    for (unsigned int i = 0; i < num_parts; i++) {
        Key_entry* key = partitions[i].head;
        while (key != NULL) {
            // Free all values for this key
            Value_node* val = key->values;
            while (val != NULL) {
                Value_node* next_val = val->next;
                free(val->value);  // Free value string
                free(val);         // Free value node
                val = next_val;
            }
            
            Key_entry* next_key = key->next;
            free(key->key);  // Free key string
            free(key);       // Free key entry
            key = next_key;
        }
        
        pthread_mutex_destroy(&partitions[i].partition_mutex);
    }
    
    free(partitions);
    free(iterators);
    partitions = NULL;
    iterators = NULL;
}