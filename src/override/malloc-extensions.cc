#include "malloc-extensions.h"

#include "../snmalloc.h"

using namespace snmalloc;

void get_malloc_info_v1(malloc_info_v1* stats)
{
  auto next_memory_usage = default_memory_provider().memory_usage();
  stats->current_memory_usage = next_memory_usage.first;
  stats->peak_memory_usage = next_memory_usage.second;
}
#include <iostream>
void get_malloc_info_x1(malloc_info_x1* stats)
{
  auto next_memory_usage = default_memory_provider().memory_usage();
  stats->system_allocated_bytes = next_memory_usage.second;
  stats->application_allocated_bytes = 0;
  stats->allocations_small = 0;
  stats->allocations_medium = 0;
  stats->allocations_large = 0;

  Stats internal_stats = {};
  current_alloc_pool()->aggregate_stats(internal_stats);
  stats->application_requested_bytes = internal_stats.requested_bytes_guage;

  for (sizeclass_t i = 0; i < NUM_SIZECLASSES; i++)
  {
    size_t count = internal_stats.sizeclass[i];
    stats->application_allocated_bytes += count * sizeclass_to_size(i);
    if (i < NUM_SMALL_CLASSES) {
      stats->allocations_small += count;
    } else {
      stats->allocations_medium += count;
    }
  }

  for (uint8_t i = 0; i < NUM_LARGE_CLASSES; i++)
  {
    size_t count = internal_stats.large_pop_count[i] - internal_stats.large_push_count[i];
    stats->application_allocated_bytes += count * large_sizeclass_to_size(i);
    stats->allocations_large += count; 
  }
}