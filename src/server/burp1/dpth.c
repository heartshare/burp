#include "include.h"
#include "../server/sdirs.h"

#include <dirent.h>

void mk_dpthl(struct dpthl *dpthl, struct conf *cconf, char cmd)
{
	// File data.
	snprintf(dpthl->path, sizeof(dpthl->path), "%04X/%04X/%04X%s",
		dpthl->prim, dpthl->seco, dpthl->tert,
		// Because of the way EFS works, it cannot be compressed.
		(cconf->compression && cmd!=CMD_EFS_FILE)?".gz":"");
}

static void mk_dpthl_prim(struct dpthl *dpthl)
{
	snprintf(dpthl->path, sizeof(dpthl->path), "%04X", dpthl->prim);
}

static void mk_dpthl_seco(struct dpthl *dpthl)
{
	snprintf(dpthl->path, sizeof(dpthl->path), "%04X/%04X",
		dpthl->prim, dpthl->seco);
}

static int get_highest_entry(const char *path)
{
	int ent=0;
	int max=0;
	DIR *d=NULL;
	struct dirent *dp=NULL;

	if(!(d=opendir(path))) return -1;
	while((dp=readdir(d)))
	{
		if(dp->d_ino==0
		  || !strcmp(dp->d_name, ".")
		  || !strcmp(dp->d_name, ".."))
			continue;
		ent=strtol(dp->d_name, NULL, 16);
		if(ent>max) max=ent;
	}
	closedir(d);
	return max;
}

// -1 for error.
// 1 if the directory did not exist yet.
// 0 to continue processing the components.
static int get_next_comp(const char *currentdata, const char *path, int *comp)
{
	char *tmp=NULL;
	if(path)
		tmp=prepend_s(currentdata, path);
	else
		tmp=strdup_w(currentdata, __func__);
	if(!tmp) return -1;
	if((*comp=get_highest_entry(tmp))<0)
	{
		// Could not open directory. Set zero.
		*comp=0;
		free_w(&tmp);
		return 1;
	}
	free_w(&tmp);
	return 0;
}

int init_dpthl(struct dpthl *dpthl, struct sdirs *sdirs, struct conf *cconf)
{
	int ret=0;
	dpthl->prim=0;
	dpthl->seco=0;
	dpthl->tert=0;
	*(dpthl->path)='\0';

	if((ret=get_next_comp(sdirs->currentdata, dpthl->path, &dpthl->prim)))
		goto end;

	mk_dpthl_prim(dpthl);
	if((ret=get_next_comp(sdirs->currentdata, dpthl->path, &dpthl->seco)))
		goto end;

	mk_dpthl_seco(dpthl);
	if((ret=get_next_comp(sdirs->currentdata, dpthl->path, &dpthl->tert)))
		goto end;

	// At this point, we have the latest data file. Increment to get the
	// next free one.
	ret=incr_dpthl(dpthl, cconf);

end:
	switch(ret)
	{
		case -1: return -1;
		default: return 0;
	}
}

// Three levels with 65535 entries each gives
// 65535^3 = 281,462,092,005,375 data entries
// recommend a filesystem with lots of inodes?
// Hmm, but ext3 only allows 32000 subdirs, although that many files are OK.
int incr_dpthl(struct dpthl *dpthl, struct conf *cconf)
{
	if(dpthl->tert++<0xFFFF) return 0;
	dpthl->tert=0;

	if(dpthl->seco++<cconf->max_storage_subdirs) return 0;
	dpthl->seco=0;

	if(dpthl->prim++<cconf->max_storage_subdirs) return 0;
	dpthl->prim=0;

	logp("No free data file entries out of the 15000*%d*%d available!\n",
		cconf->max_storage_subdirs, cconf->max_storage_subdirs);
	logp("Recommend moving the client storage directory aside and starting again.\n");
	return -1;
}

int set_dpthl_from_string(struct dpthl *dpthl,
	const char *datapath, struct conf *cconf)
{
	unsigned int a=0;
	unsigned int b=0;
	unsigned int c=0;

	if(!datapath
	  || *datapath=='t') // The path used the tree style structure.
		return 0;

	if((sscanf(datapath, "%04X/%04X/%04X", &a, &b, &c))!=3)
		return -1;

	// Only set it if it is a higher one.
	if(dpthl->prim > (int)a
	  || dpthl->seco > (int)b
	  || dpthl->tert > (int)c) return 0;

	dpthl->prim=a;
	dpthl->seco=b;
	dpthl->tert=c;
	return 0;
}
