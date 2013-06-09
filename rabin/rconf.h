#ifndef __RCONF_H
#define __RCONF_H

#include <stdint.h>

struct rconf
{
	uint64_t prime;

	uint32_t win_min;
	uint32_t win;
	uint32_t win_max;
	
	uint32_t blk_min;
	uint32_t blk_avg;
	uint32_t blk_max;
};

extern void rconf_init(struct rconf *rconf);
extern int rconf_check(struct rconf *rconf);

#endif
