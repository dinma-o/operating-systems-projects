/**
 * threadpool.c - Thread Pool Library Implementation
 * 
 * This library implements a fixed-size thread pool with:
 * - Shortest Job First (SJF) scheduling policy
 * - POSIX mutex and condition variable synchronization
 * - Barrier synchronization for phase completion
 * 
 * Key Components:
 * - Worker threads that continuously process jobs from a queue
 * - SJF-ordered job queue (sorted by job size)
 * - Synchronization primitives for thread-safe operations
 */

#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

/**
 * Enhanced ThreadPool structure with synchronization primitives
 */
typedef struct ThreadPool_work {
    pthread_t* threads;              // Array of worker thread handles
    ThreadPool_job_queue_t jobs;     // Job queue (SJF-ordered)
    
    // SYNCHRONIZATION PRIMITIVES
    pthread_mutex_t queue_mutex;     // Protects job queue and state variables
    pthread_cond_t jobs_available;   // Signals when jobs are added to queue
    pthread_cond_t jobs_done;        // Signals when all work is complete
    
    // STATE TRACKING
    unsigned int num_threads;        // Total number of worker threads
    unsigned int active_threads;     // Number of threads currently executing jobs
    unsigned int pending_jobs;       // Number of jobs in queue (same as jobs.size)
    bool shutdown;                   // Flag to signal threads to exit
} ThreadPool_work;

/**
 * ThreadPool_create - Initialize and create a thread pool
 * 
 * @param num: Number of worker threads to create
 * @return: Pointer to newly created ThreadPool_t object, NULL on failure
 * 
 * Process:
 * 1. Allocate ThreadPool structure
 * 2. Initialize all synchronization primitives
 * 3. Initialize state variables
 * 4. Create worker threads
 * 
 * Synchronization: None needed (called before threads start)
 */
ThreadPool_t* ThreadPool_create(unsigned int num) {
    // Allocate and zero-initialize the thread pool structure
    ThreadPool_work* tp = (ThreadPool_work*)calloc(1, sizeof(ThreadPool_work));
    if (tp == NULL) {
        return NULL;
    }
    
    // Initialize synchronization primitives
    // queue_mutex: protects job queue and all state variables
    pthread_mutex_init(&tp->queue_mutex, NULL);
    
    // jobs_available: condition variable to wake threads when jobs are added
    pthread_cond_init(&tp->jobs_available, NULL);
    
    // jobs_done: condition variable to signal when all work is complete
    // (used by ThreadPool_check as a barrier)
    pthread_cond_init(&tp->jobs_done, NULL);
    
    // Initialize state variables
    tp->num_threads = num;
    tp->active_threads = 0;
    tp->pending_jobs = 0;
    tp->shutdown = false;
    
    // Initialize job queue
    tp->jobs.head = NULL;
    tp->jobs.size = 0;
    
    // Allocate array for thread handles
    tp->threads = (pthread_t*)malloc(num * sizeof(pthread_t));
    if (tp->threads == NULL) {
        // Cleanup on allocation failure
        pthread_mutex_destroy(&tp->queue_mutex);
        pthread_cond_destroy(&tp->jobs_available);
        pthread_cond_destroy(&tp->jobs_done);
        free(tp);
        return NULL;
    }
    
    // Create worker threads
    // Each thread runs Thread_run with tp as argument
    for (unsigned int i = 0; i < num; i++) {
        if (pthread_create(&tp->threads[i], NULL, 
                          (void* (*)(void*))Thread_run, tp) != 0) {
            // If thread creation fails, set shutdown and clean up
            tp->shutdown = true;
            // Join any threads that were created
            for (unsigned int j = 0; j < i; j++) {
                pthread_join(tp->threads[j], NULL);
            }
            pthread_mutex_destroy(&tp->queue_mutex);
            pthread_cond_destroy(&tp->jobs_available);
            pthread_cond_destroy(&tp->jobs_done);
            free(tp->threads);
            free(tp);
            return NULL;
        }
    }
    
    return (ThreadPool_t*)tp;
}

/**
 * ThreadPool_add_job - Add a job to the thread pool's queue
 * 
 * @param tp: Pointer to the ThreadPool object
 * @param func: Function pointer to execute
 * @param arg: Arguments for the function
 * @param job_size: Size metric for SJF scheduling (file size or partition size)
 * @return: true on success, false on failure
 * 
 * SJF Implementation:
 * - Jobs are inserted into a sorted linked list
 * - Maintained in ascending order by job_size
 * - Smallest jobs at the head (dequeued first)
 * 
 * Synchronization:
 * - Acquires queue_mutex before modifying queue
 * - Signals jobs_available to wake one waiting thread
 * - Releases queue_mutex after insertion
 */
bool ThreadPool_add_job(ThreadPool_t* tp, thread_func_t func, 
                       void* arg, unsigned long job_size) {
    ThreadPool_work* pool = (ThreadPool_work*)tp;
    
    // Allocate new job node
    ThreadPool_job_t* new_job = (ThreadPool_job_t*)malloc(sizeof(ThreadPool_job_t));
    if (new_job == NULL) {
        return false;
    }
    
    // Initialize job fields
    new_job->func = func;
    new_job->arg = arg;
    new_job->job_size = job_size;
    new_job->next = NULL;
    
    // CRITICAL SECTION: Modify job queue
    pthread_mutex_lock(&pool->queue_mutex);
    
    // SJF INSERTION: Maintain sorted order (ascending by job_size)
    if (pool->jobs.head == NULL || job_size < pool->jobs.head->job_size) {
        // Insert at head (smallest job so far)
        new_job->next = pool->jobs.head;
        pool->jobs.head = new_job;
    } else {
        // Find insertion point: first node where next->job_size > job_size
        ThreadPool_job_t* current = pool->jobs.head;
        while (current->next != NULL && 
               current->next->job_size <= job_size) {
            current = current->next;
        }
        // Insert after current
        new_job->next = current->next;
        current->next = new_job;
    }
    
    // Update queue statistics
    pool->jobs.size++;
    pool->pending_jobs++;
    
    // Wake up ONE waiting thread to process this job
    // Using signal (not broadcast) for efficiency
    pthread_cond_signal(&pool->jobs_available);
    
    pthread_mutex_unlock(&pool->queue_mutex);
    // END CRITICAL SECTION
    
    return true;
}

/**
 * ThreadPool_get_job - Remove and return the next job from the queue
 * 
 * @param tp: Pointer to the ThreadPool object
 * @return: Pointer to next job, or NULL if queue is empty
 * 
 * SJF Dequeue:
 * - Always removes from head (smallest job)
 * - Caller must hold queue_mutex
 * 
 * Synchronization:
 * - CALLER MUST HOLD queue_mutex before calling
 * - Updates queue size and pending_jobs count
 */
ThreadPool_job_t* ThreadPool_get_job(ThreadPool_t* tp) {
    ThreadPool_work* pool = (ThreadPool_work*)tp;
    
    // PRECONDITION: Caller holds queue_mutex
    
    if (pool->jobs.head == NULL) {
        return NULL;
    }
    
    // Remove job from head (shortest job due to SJF ordering)
    ThreadPool_job_t* job = pool->jobs.head;
    pool->jobs.head = job->next;
    pool->jobs.size--;
    pool->pending_jobs--;
    
    return job;
}

/**
 * Thread_run - Main loop for worker threads
 * 
 * @param tp: Pointer to the ThreadPool object
 * @return: NULL (required by pthread_create)
 * 
 * Worker Thread State Machine:
 * 1. Wait for jobs or shutdown signal
 * 2. Get job from queue (if available)
 * 3. Execute job
 * 4. Signal completion (if all work done)
 * 5. Repeat
 * 
 * Synchronization:
 * - Uses pthread_cond_wait for efficient sleeping
 * - Rechecks conditions after waking (spurious wakeups)
 * - Updates active_threads atomically
 * - Signals jobs_done when work complete
 * 
 * Correctness:
 * - No race conditions: all shared state accessed under mutex
 * - No deadlocks: mutex always released, no circular waits
 * - No missed signals: condition checked in while loop
 */
void* Thread_run(ThreadPool_t* tp) {
    ThreadPool_work* pool = (ThreadPool_work*)tp;
    
    while (1) {
        // CRITICAL SECTION: Check queue state
        pthread_mutex_lock(&pool->queue_mutex);
        
        // Check for shutdown signal
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->queue_mutex);
            pthread_exit(NULL);
        }
        
        // Wait for jobs if queue is empty
        // pthread_cond_wait atomically releases mutex and sleeps
        // It reacquires mutex before returning
        while (pool->jobs.head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->jobs_available, &pool->queue_mutex);
        }
        
        // Recheck shutdown after waking (could be shutdown signal)
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->queue_mutex);
            pthread_exit(NULL);
        }
        
        // Get next job from queue (SJF: always from head)
        ThreadPool_job_t* job = ThreadPool_get_job(tp);
        
        // Mark this thread as active
        pool->active_threads++;
        
        pthread_mutex_unlock(&pool->queue_mutex);
        // END CRITICAL SECTION
        
        // Execute job OUTSIDE critical section (allows other threads to work)
        if (job != NULL) {
            job->func(job->arg);
            free(job);  // Free job node after execution
        }
        
        // CRITICAL SECTION: Update completion state
        pthread_mutex_lock(&pool->queue_mutex);
        
        // Mark this thread as idle
        pool->active_threads--;
        
        // If all work is done, signal the barrier (ThreadPool_check)
        if (pool->active_threads == 0 && pool->pending_jobs == 0) {
            pthread_cond_signal(&pool->jobs_done);
        }
        
        pthread_mutex_unlock(&pool->queue_mutex);
        // END CRITICAL SECTION
    }
    
    return NULL;
}

/**
 * ThreadPool_check - Barrier synchronization
 * 
 * @param tp: Pointer to the ThreadPool object
 * 
 * Purpose:
 * - Block until ALL jobs are complete
 * - Ensures no jobs in queue AND no threads executing
 * - Used to separate Map and Reduce phases
 * 
 * Synchronization:
 * - Waits on jobs_done condition variable
 * - Worker threads signal when work complete
 * - Handles spurious wakeups with while loop
 * 
 * Correctness:
 * - No deadlock: pthread_cond_wait releases mutex while waiting
 * - No missed signals: condition checked in while loop
 * - Barrier semantics: guaranteed all work done before return
 */
void ThreadPool_check(ThreadPool_t* tp) {
    ThreadPool_work* pool = (ThreadPool_work*)tp;
    
    pthread_mutex_lock(&pool->queue_mutex);
    
    // Wait until no pending jobs AND no active threads
    while (pool->pending_jobs > 0 || pool->active_threads > 0) {
        // pthread_cond_wait releases mutex and sleeps
        // Reacquires mutex when signaled
        pthread_cond_wait(&pool->jobs_done, &pool->queue_mutex);
    }
    
    pthread_mutex_unlock(&pool->queue_mutex);
}

/**
 * ThreadPool_destroy - Clean up and destroy the thread pool
 * 
 * @param tp: Pointer to the ThreadPool object
 * 
 * Graceful Shutdown Process:
 * 1. Set shutdown flag
 * 2. Wake all sleeping threads
 * 3. Wait for all threads to exit
 * 4. Clean up remaining resources
 * 
 * Synchronization:
 * - Uses broadcast to wake all threads
 * - Joins all threads (blocks until exit)
 * - Destroys all sync primitives
 * 
 * Memory Management:
 * - Frees any remaining jobs in queue
 * - Frees thread handle array
 * - Frees ThreadPool structure
 */
void ThreadPool_destroy(ThreadPool_t* tp) {
    ThreadPool_work* pool = (ThreadPool_work*)tp;
    
    // CRITICAL SECTION: Set shutdown flag
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = true;
    
    // Wake ALL sleeping threads so they see shutdown flag
    // Using broadcast (not signal) to wake all threads
    pthread_cond_broadcast(&pool->jobs_available);
    pthread_mutex_unlock(&pool->queue_mutex);
    // END CRITICAL SECTION
    
    // Wait for all threads to exit cleanly
    for (unsigned int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    // Clean up synchronization primitives
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->jobs_available);
    pthread_cond_destroy(&pool->jobs_done);
    
    // Free any remaining jobs in queue (shouldn't be any if used correctly)
    ThreadPool_job_t* current = pool->jobs.head;
    while (current != NULL) {
        ThreadPool_job_t* next = current->next;
        free(current);  // Note: not freeing arg (user's responsibility)
        current = next;
    }
    
    // Free thread handle array
    free(pool->threads);
    
    // Free thread pool structure
    free(pool);
}