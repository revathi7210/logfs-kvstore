/**
 * Tony Givargis
 * Copyright (C), 2023 - 2024
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * logfs.c
 */

#include "logfs.h"

#include <pthread.h>
#include <string.h>

#include "device.h"

#define WCACHE_BLOCKS 32
#define RCACHE_BLOCKS 256

/**
 * Needs:
 *   pthread_create()
 *   pthread_join()
 *   pthread_mutex_init()
 *   pthread_mutex_destroy()
 *   pthread_mutex_lock()
 *   pthread_mutex_unlock()
 *   pthread_cond_init()
 *   pthread_cond_destroy()
 *   pthread_cond_wait()
 *   pthread_cond_signal()
 */

/* research the above Needed API and design accordingly */

struct RcacheBlock {
    int valid;
    int block_id;
    char *block;
    char *block_; /*not aligned*/
};

struct logfs {
    struct device *device; /*from device.c*/
    char *write_buffer_add;
    char *write_buffer_add_raw;
    int head;    /*for write function*/
    int tail;    /*for write function*/
    pthread_mutex_t mutex;
    pthread_cond_t ready_to_write;
    pthread_cond_t flush_successful;
    int block_size;
    int write_buffer_size;
    int write_buffer_filled;
    int exit_worker_thread;
    int device_offset;
    pthread_t worker_thread;
    struct RcacheBlock *read_buff[RCACHE_BLOCKS];
};


void *write_to_disk(void *args) {
    struct logfs *logfs = (struct logfs *)args;
    int block_id;
    int read_cache_idx;

    /* Lock the mutex*/
    if (pthread_mutex_lock(&logfs->mutex) != 0) {
        TRACE("Failed to lock mutex");
        pthread_exit(NULL);
    }

    while (!logfs->exit_worker_thread) {

        /* Wait until there is enough data to write*/
        while (logfs->write_buffer_filled < logfs->block_size) {
            if (logfs->exit_worker_thread) {
                if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                    TRACE("Failed to unlock mutex");
                }
                pthread_exit(NULL);
            }

            /* Wait for the signal indicating that the write buffer has data*/
            if (pthread_cond_wait(&logfs->ready_to_write, &logfs->mutex) != 0) {
                TRACE("Failed to wait on condition variable");
                if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                    TRACE("Failed to unlock mutex after condition wait failure");
                }
                pthread_exit(NULL);
            }
        }

        block_id = logfs->device_offset / logfs->block_size;

        /* Determine the index of the read cache to check if the block is already cached */
        read_cache_idx = block_id % RCACHE_BLOCKS;

        /* If the block is found in the read cache, invalidate it before writing */
        if (logfs->read_buff[read_cache_idx]->block_id == block_id) {
            if (logfs->read_buff[read_cache_idx] == NULL) {
                TRACE("Read buffer entry is NULL");
                if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                    TRACE("Failed to unlock mutex");
                }
                pthread_exit(NULL);
            }
            logfs->read_buff[read_cache_idx]->valid = 0;
        }

        if (device_write(logfs->device, logfs->write_buffer_add + logfs->tail,
                         logfs->device_offset, logfs->block_size)) {
            TRACE("Failed to write to disk");
            if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                TRACE("Failed to unlock mutex after write error");
            }
            pthread_exit(NULL);
        }

        /* Update the tail pointer->moving it forward by the size of one block */
        logfs->tail = (logfs->tail + logfs->block_size) % logfs->write_buffer_size;

        /* Decrease the amount of data currently filled*/
        logfs->write_buffer_filled -= logfs->block_size;

        /* Increment the device offset to reflect the data written*/
        logfs->device_offset += logfs->block_size;

        /* If the head and tail pointers are the same or the tail has wrapped around*/
        if (logfs->head == logfs->tail ||
            (logfs->tail == 0 && logfs->head == logfs->write_buffer_size)) {
            if (pthread_cond_signal(&logfs->flush_successful) != 0) {
                TRACE("Failed to signal flush successful");
                if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                    TRACE("Failed to unlock mutex after signal error");
                }
                pthread_exit(NULL);
            }
        }
    }

    if (pthread_mutex_unlock(&logfs->mutex) != 0) {
        TRACE("Failed to unlock mutex during exit");
    }
    pthread_exit(NULL);
}



int flush_to_disk(struct logfs *logfs) {
    int head_advance_distance;

    if (pthread_mutex_lock(&logfs->mutex) != 0) {
        TRACE("Failed to lock mutex");
        return -1;
    }

    /* Calculate how much to move the head pointer to align with the next block boundary */
    head_advance_distance = logfs->block_size - (logfs->head % logfs->block_size);

    /* Move the head and update write buffer fill level accordingly */
    logfs->head += head_advance_distance;
    logfs->write_buffer_filled += head_advance_distance;

    /* Notify the worker thread that data is ready to be written */
    if (pthread_cond_signal(&logfs->ready_to_write) != 0) {
        TRACE("Failed to signal ready_to_write");
        pthread_mutex_unlock(&logfs->mutex);
        return -1;
    }

    /* Wait for the worker thread to complete the flush operation */
    if (pthread_cond_wait(&logfs->flush_successful, &logfs->mutex) != 0) {
        TRACE("Failed to wait on flush_successful condition");
        pthread_mutex_unlock(&logfs->mutex);
        return -1;
    }

    /* Restore the head pointer to its previous position after the flush */
    logfs->head -= head_advance_distance;

    /* Adjust the tail pointer based on the head's new position */
    if (logfs->tail == 0) {
        logfs->tail = logfs->head - (logfs->head % logfs->block_size);
    } else {
        logfs->tail -= logfs->block_size;
    }

    /* Update write buffer fill level */
    logfs->write_buffer_filled = logfs->head % logfs->block_size;

    /* Adjust device offset to reflect the completed flush */
    logfs->device_offset -= logfs->block_size;

    if (pthread_mutex_unlock(&logfs->mutex) != 0) {
        TRACE("Failed to unlock mutex");
        return -1;
    }

    return 0;
}



struct logfs *logfs_open(const char *pathname) {
    struct logfs *logfs;
    int i,j;

    if (!(logfs = (struct logfs *)malloc(sizeof(struct logfs)))) { /* Allocate memory for the logfs structure */
        TRACE("Failed to allocate memory for logfs");
        return NULL;
    }

    if (!(logfs->device = device_open(pathname))) { /* Open the device*/
        TRACE("Failed to open device");
        free(logfs);
        return NULL;
    }

    logfs->block_size = device_block(logfs->device); /* Retrieve the block size for the device */
    if (logfs->block_size <= 0) {
        TRACE("Invalid block size");
        device_close(logfs->device);
        free(logfs);
        return NULL;
    }

    /* Calculate the write buffer size and allocate memory for it */
    logfs->write_buffer_size = WCACHE_BLOCKS * logfs->block_size;
    logfs->write_buffer_add_raw = (char *)malloc(logfs->write_buffer_size + logfs->block_size);
    if (!logfs->write_buffer_add_raw) {
        TRACE("Failed to allocate memory for write buffer");
        device_close(logfs->device);
        free(logfs);
        return NULL;
    }

    /* Align the write buffer to the device block size */
    logfs->write_buffer_add = (char *)memory_align(logfs->write_buffer_add_raw, logfs->block_size);
    if (!logfs->write_buffer_add) {
        TRACE("Failed to align write buffer");
        free(logfs->write_buffer_add_raw);
        device_close(logfs->device);
        free(logfs);
        return NULL;
    }

    memset(logfs->write_buffer_add, 0, logfs->write_buffer_size);
    logfs->head = 0;
    logfs->tail = 0;
    logfs->write_buffer_filled = 0;
    logfs->exit_worker_thread = 0;
    logfs->device_offset = 0;

    for (i = 0; i < RCACHE_BLOCKS; ++i) {
        logfs->read_buff[i] = (struct RcacheBlock *)malloc(sizeof(struct RcacheBlock));
        if (!logfs->read_buff[i]) {
            TRACE("Failed to allocate memory for read buffer entry");
            goto cleanup_read_buff;
        }

        /* Allocate memory for the block data*/
        logfs->read_buff[i]->block_ = (char *)malloc(2 * logfs->block_size);
        if (!logfs->read_buff[i]->block_) {
            TRACE("Failed to allocate memory for read buffer block");
            free(logfs->read_buff[i]);
            goto cleanup_read_buff;
        }

        /* Align the block data to the block size */
        logfs->read_buff[i]->block = (char *)memory_align(logfs->read_buff[i]->block_, logfs->block_size);
        if (!logfs->read_buff[i]->block) {
            TRACE("Failed to align read buffer block");
            free(logfs->read_buff[i]->block_);
            free(logfs->read_buff[i]);
            goto cleanup_read_buff;
        }

        logfs->read_buff[i]->block_id = -1;
        logfs->read_buff[i]->valid = 0;
    }

    /* Initialize mutex and condition variables for thread synchronization */
    if (pthread_mutex_init(&logfs->mutex, NULL) != 0 ||
        pthread_cond_init(&logfs->ready_to_write, NULL) != 0 ||
        pthread_cond_init(&logfs->flush_successful, NULL) != 0) {
        TRACE("Failed to initialize mutex or condition variables");
        goto cleanup_read_buff;
    }

    /* Create the worker thread responsible for writing data to disk */
    if (pthread_create(&logfs->worker_thread, NULL, &write_to_disk, logfs) != 0) {
        TRACE("Failed to create worker thread");
        goto cleanup_sync;
    }

    return logfs;

/* Cleanup in case of errors */
cleanup_sync:
    pthread_mutex_destroy(&logfs->mutex);
    pthread_cond_destroy(&logfs->ready_to_write);
    pthread_cond_destroy(&logfs->flush_successful);

cleanup_read_buff:
    for ( j = 0; j < i; ++j) {
        if (logfs->read_buff[j]) {
            free(logfs->read_buff[j]->block_);
            free(logfs->read_buff[j]);
        }
    }
    free(logfs->write_buffer_add_raw);
    device_close(logfs->device);
    free(logfs);
    return NULL;
}


int logfs_append(struct logfs *logfs, const void *buf, uint64_t len) {

    uint64_t bytes_to_write = len;

    if (pthread_mutex_lock(&logfs->mutex) != 0) {
        TRACE("Failed to lock mutex");
        return -1;
    }

    while (bytes_to_write > 0) {
        

        /* Ensure the write buffer is not overfilled */
        if (logfs->write_buffer_filled >= logfs->write_buffer_size) {
            TRACE("Write buffer is full");
            if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                TRACE("Failed to unlock mutex after buffer overflow");
            }
            return -1;
        }

        /* Copy the current byte from the input buffer to the write buffer at the current head position */
        if (!memcpy(logfs->write_buffer_add + logfs->head,
                    (char *)buf + (len - bytes_to_write), 1)) {
            TRACE("Failed to copy data to write buffer");
            if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                TRACE("Failed to unlock mutex after memcpy failure");
            }
            return -1;
        }

        /* Increment the number of bytes filled in the write buffer */
        logfs->write_buffer_filled++;

        /* Decrement the remaining bytes to write */
        bytes_to_write--;

        /* Update the head pointer, wrapping it around the write buffer size */
        logfs->head = (logfs->head + 1) % logfs->write_buffer_size;

        /* Signal the worker thread that there is data ready to be written to disk */
        if (pthread_cond_signal(&logfs->ready_to_write) != 0) {
            TRACE("Failed to signal ready_to_write");
            if (pthread_mutex_unlock(&logfs->mutex) != 0) {
                TRACE("Failed to unlock mutex after signal failure");
            }
            return -1;
        }
    }

    /* Unlock the mutex after modifying the logfs structure */
    if (pthread_mutex_unlock(&logfs->mutex) != 0) {
        TRACE("Failed to unlock mutex");
        return -1;
    }

    return 0;
}


int logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len) {
    int block_id;
    int read_buffer_idx;
    int read_start_off;
    int len_to_read;
    size_t read_till_now = 0;

    /* Ensure that any data pending to be written to disk is flushed */
    if (flush_to_disk(logfs) != 0) {
        TRACE("Failed to flush data to disk");
        return -1;
    }

    /* Calculate the block ID for the requested offset */
    block_id = off / logfs->block_size;

    read_buffer_idx = block_id % RCACHE_BLOCKS;
    read_start_off = off % logfs->block_size;
    len_to_read = MIN(len, (size_t)logfs->block_size - read_start_off);


    while (read_till_now < len) {
        /* Check if the block is already in the read cache and valid */
        if (logfs->read_buff[read_buffer_idx] != NULL &&
            logfs->read_buff[read_buffer_idx]->valid &&
            logfs->read_buff[read_buffer_idx]->block_id == block_id) {

            /* Copy data from the read cache to the user buffer */
            if (!memcpy((char *)buf + read_till_now,
                        logfs->read_buff[read_buffer_idx]->block + read_start_off,
                        len_to_read)) {
                TRACE("Failed to copy data from read buffer");
                return -1;
            }
        } else {
            /* If not in cache, read the block from the device */
            if (device_read(logfs->device, logfs->read_buff[read_buffer_idx]->block,
                            block_id * logfs->block_size, logfs->block_size)) {
                TRACE("Error while calling device_read");
                return -1;
            }

            /* Mark the block as valid in the cache and store the block ID */
            logfs->read_buff[read_buffer_idx]->valid = 1;
            logfs->read_buff[read_buffer_idx]->block_id = block_id;

            /* Copy data from the newly-read block into the user buffer */
            if (!memcpy((char *)buf + read_till_now,
                        logfs->read_buff[read_buffer_idx]->block + read_start_off,
                        len_to_read)) {
                TRACE("Failed to copy data from device read");
                return -1;
            }
        }

        /* Update the amount of data read so far */
        read_till_now += len_to_read;

        /* Move to the next block */
        block_id++;

        /* Update the read buffer index for the next block */
        read_buffer_idx = block_id % RCACHE_BLOCKS;
        read_start_off = 0;
        len_to_read = MIN((size_t)logfs->block_size, len - read_till_now);
    }

    return 0;
}


void logfs_close(struct logfs *logfs) {
    int i;

    /* Ensure any pending data is flushed to disk */
    if (flush_to_disk(logfs) != 0) {
        TRACE("Failed to flush data to disk during close");
    }

    /* Lock the mutex to safely modify the logfs structure */
    if (pthread_mutex_lock(&logfs->mutex) != 0) {
        TRACE("Failed to lock mutex during logfs close");
        return;
    }

    /* Set the exit flag to indicate the worker thread should stop */
    logfs->exit_worker_thread = 1;

    /* Unlock the mutex after modifying the logfs structure */
    if (pthread_mutex_unlock(&logfs->mutex) != 0) {
        TRACE("Failed to unlock mutex during logfs close");
        return;
    }

    /* Signal the worker thread to continue and exit */
    if (pthread_cond_signal(&logfs->ready_to_write) != 0) {
        TRACE("Failed to signal worker thread during logfs close");
        return;
    }

    /* Wait for the worker thread to complete */
    if (pthread_join(logfs->worker_thread, NULL) != 0) {
        TRACE("Failed to join worker thread during logfs close");
        return;
    }

    /* Free the read buffer memory */
    for (i = 0; i < RCACHE_BLOCKS; ++i) {
        FREE(logfs->read_buff[i]->block_);
        FREE(logfs->read_buff[i]);
    }

    /* Free the write buffer memory */
    FREE(logfs->write_buffer_add_raw);

    /* Close the device */
    device_close(logfs->device);

    /* Free the logfs structure memory */
    FREE(logfs);
}
