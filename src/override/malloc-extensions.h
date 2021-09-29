/**
 * Malloc extensions
 *
 * This file contains additional non-standard API surface for snmalloc.
 * The API is subject to changes, but will be clearly noted in release
 * notes.
 */

/**
 * Structure for returning memory used by snmalloc.
 *
 * The statistics are very coarse grained as they only track
 * usage at the superslab/chunk level. Meta-data and object
 * data is not tracked independantly.
 */
struct malloc_info_v1
{
  /**
   * Current memory usage of the allocator. Extremely coarse
   * grained for efficient calculation.
   */
  size_t current_memory_usage;

  /**
   * High-water mark of current_memory_usage.
   */
  size_t peak_memory_usage;
};

struct malloc_info_x1
{
  size_t system_allocated_bytes;
  size_t application_requested_bytes;
  size_t application_allocated_bytes;
  size_t allocations_small;
  size_t allocations_medium;
  size_t allocations_large;
};

/**
 * Populates a malloc_info_v1 structure for the latest values
 * from snmalloc.
 */
void get_malloc_info_v1(malloc_info_v1* stats);

void get_malloc_info_x1(malloc_info_x1* stats);