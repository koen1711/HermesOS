/*
Copyright (C) 2015-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.
*/

#ifndef PAGE_H
#define PAGE_H

#include "kernel/types.h"

void  page_init(uint64_t total_memory);
void *page_alloc(bool zeroit);
void  page_free(void *addr);
void  page_stats( uint64_t *nfree, uint64_t *ntotal );

#endif
