#include "include.h"
#include "champ_chooser/include.h"
#include "../../server/manio.h"
#include "../../burp2/blist.h"
#include "../../burp2/slist.h"

static int data_needed(struct sbuf *sb)
{
	if(sb->path.cmd==CMD_FILE) return 1;
	return 0;
}

// Return -1 for error, 0 for entry not changed, 1 for entry changed (or new).
static int found_in_current_manifest(struct asfd *asfd,
	struct sbuf *csb, struct sbuf *sb,
	struct manio *cmanio, struct manio *unmanio,
	struct blk **blk, struct conf *conf)
{
	// Located the entry in the current manifest.
	// If the file type changed, I think it is time to back it up again
	// (for example, EFS changing to normal file, or back again).
	if(csb->path.cmd!=sb->path.cmd)
	{
		if(manio_forward_through_sigs(asfd, &csb, blk, cmanio, conf)<0)
			return -1;
		return 1;
	}

	// mtime is the actual file data.
	// ctime is the attributes or meta data.
	if(csb->statp.st_mtime==sb->statp.st_mtime
	  && csb->statp.st_ctime==sb->statp.st_ctime)
	{
		// Got an unchanged file.
		if(manio_copy_entry(asfd, &csb, sb,
			blk, cmanio, unmanio, conf)<0) return -1;
		return 0;
	}

	if(csb->statp.st_mtime==sb->statp.st_mtime
	  && csb->statp.st_ctime!=sb->statp.st_ctime)
	{
		// File data stayed the same, but attributes or meta data
		// changed. We already have the attributes, but may need to
		// get extra meta data.
		// FIX THIS
		if(manio_copy_entry(asfd, &csb, sb,
			blk, cmanio, unmanio, conf)<0) return -1;
		return 0;
	}

	// File data changed.
	if(manio_forward_through_sigs(asfd, &csb, blk, cmanio, conf)<0)
		return -1;
	return 1;
}

// Return -1 for error, 0 for entry not changed, 1 for entry changed (or new).
static int entry_changed(struct asfd *asfd, struct sbuf *sb,
	struct manio *cmanio, struct manio *unmanio, struct conf *conf)
{
	static int finished=0;
	static struct sbuf *csb=NULL;
	static struct blk *blk=NULL;

	if(finished) return 1;

	if(!csb && !(csb=sbuf_alloc(conf))) return -1;

	if(csb->path.buf)
	{
		// Already have an entry.
	}
	else
	{
		// Need to read another.
		if(!blk && !(blk=blk_alloc())) return -1;
		switch(manio_sbuf_fill(cmanio, asfd, csb, blk, NULL, conf))
		{
			case 1: // Reached the end.
				sbuf_free(&csb);
				blk_free(&blk);
				finished=1;
				return 1;
			case -1: return -1;
		}
		if(!csb->path.buf)
		{
			logp("Should have a path at this point, but do not, in %s\n", __func__);
			return -1;
		}
		// Got an entry.
	}

	while(1)
	{
		switch(sbuf_pathcmp(csb, sb))
		{
			case 0: return found_in_current_manifest(asfd, csb, sb,
					cmanio, unmanio, &blk, conf);
			case 1: return 1;
			case -1:
				// Behind - need to read more data from the old
				// manifest.
				switch(manio_sbuf_fill(cmanio, asfd,
					csb, blk, NULL, conf))
				{
					case -1: return -1;
					case 1:
					{
						// Reached the end.
						sbuf_free(&csb);
						blk_free(&blk);
						return 1;
					}
				}
				// Got something, go back around the loop.
		}
	}

	return 0;
}

static int add_data_to_store(struct conf *conf,
	struct blist *blist, struct iobuf *rbuf, struct dpth *dpth)
{
	static struct blk *blk=NULL;

	// Find the first one in the list that was requested.
	// FIX THIS: Going up the list here, and then later
	// when writing to the manifest is not efficient.
	for(blk=blist->head;
		blk && (!blk->requested || blk->got==BLK_GOT); blk=blk->next)
	{
		logp("try: %d %d\n", blk->index, blk->got);
	}
	if(!blk)
	{
		logp("Received data but could not find next requested block.\n");
		if(!blist->head) logp("and blist->head is null\n");
		else logp("head index: %d\n", blist->head->index);
		return -1;
	}

	// Add it to the data store straight away.
	if(dpth_fwrite(dpth, rbuf, blk)) return -1;

	cntr_add(conf->cntr, CMD_DATA, 0);
	cntr_add_recvbytes(conf->cntr, blk->length);

	blk->got=BLK_GOT;
	blk=blk->next;

	return 0;
}

static int set_up_for_sig_info(struct slist *slist, struct blist *blist, struct sbuf *inew)
{
	struct sbuf *sb;

	for(sb=slist->add_sigs_here; sb; sb=sb->next)
	{
		if(!sb->burp2->index) continue;
		if(inew->burp2->index==sb->burp2->index) break;
	}
	if(!sb)
	{
		logp("Could not find %lu in request list %d\n",
			inew->burp2->index, sb->burp2->index);
		return -1;
	}
	// Replace the attribs with the more recent values.
	if(sb->attr.buf) free(sb->attr.buf);
	sb->attr.buf=inew->attr.buf;
	sb->attr.len=inew->attr.len;
	inew->attr.buf=NULL;

	// Mark the end of the previous file.
	slist->add_sigs_here->burp2->bend=blist->tail;

	slist->add_sigs_here=sb;

	// Incoming sigs now need to get added to 'add_sigs_here'
	return 0;
}

/*
static void dump_blks(const char *msg, struct blk *b)
{
	struct blk *xx;
	for(xx=b; xx; xx=xx->next)
		printf("%s: %d %d %p\n", msg, xx->index, xx->got, xx);
}
*/

static int add_to_sig_list(struct slist *slist, struct blist *blist,
	struct iobuf *rbuf, struct dpth *dpth, struct conf *conf)
{
	// Goes on slist->add_sigs_here
	struct blk *blk;
	struct burp2 *burp2;

	if(!(blk=blk_alloc())) return -1;
	blist_add_blk(blist, blk);

	burp2=slist->add_sigs_here->burp2;
        if(!burp2->bstart) burp2->bstart=blk;
        if(!burp2->bsighead) burp2->bsighead=blk;

	// FIX THIS: Should not just load into strings.
	if(split_sig(rbuf->buf, rbuf->len, blk->weak, blk->strong)) return -1;

	// Need to send sigs to champ chooser, therefore need to point
	// to the oldest unsent one if nothing is pointed to yet.
	if(!blist->blk_for_champ_chooser) blist->blk_for_champ_chooser=blk;

	return 0;
}

static int deal_with_read(struct iobuf *rbuf,
	struct slist *slist, struct blist *blist, struct conf *conf,
	int *sigs_end, int *backup_end, struct dpth *dpth)
{
	int ret=0;
	static struct sbuf *inew=NULL;

	if(!inew && !(inew=sbuf_alloc(conf))) goto error;

	switch(rbuf->cmd)
	{
		/* Incoming block data. */
		case CMD_DATA:
			if(add_data_to_store(conf, blist, rbuf, dpth))
				goto error;
			goto end;

		/* Incoming block signatures. */
		case CMD_ATTRIBS_SIGS:
			// New set of stuff incoming. Clean up.
			if(inew->attr.buf) free(inew->attr.buf);
			iobuf_copy(&inew->attr, rbuf);
			inew->burp2->index=decode_file_no(&inew->attr);
			rbuf->buf=NULL;

			// Need to go through slist to find the matching
			// entry.
			if(set_up_for_sig_info(slist, blist, inew)) goto error;
			return 0;
		case CMD_SIG:
			if(add_to_sig_list(slist, blist,
				rbuf, dpth, conf))
					goto error;
			goto end;

		/* Incoming control/message stuff. */
		case CMD_WARNING:
			logp("WARNING: %s\n", rbuf);
			cntr_add(conf->cntr, rbuf->cmd, 0);
			goto end;
		case CMD_GEN:
			if(!strcmp(rbuf->buf, "sigs_end"))
			{
				*sigs_end=1;
				goto end;
			}
			else if(!strcmp(rbuf->buf, "backup_end"))
			{
				*backup_end=1;
				goto end;
			}
			break;
	}

	iobuf_log_unexpected(rbuf, __func__);
error:
	ret=-1;
	sbuf_free(&inew);
end:
	if(rbuf->buf) { free(rbuf->buf); rbuf->buf=NULL; }
	return ret;
}

static int encode_req(struct blk *blk, char *req)
{
	char *p=req;
	p+=to_base64(blk->index, p);
	*p=0;
	return 0;
}

static int get_wbuf_from_sigs(struct iobuf *wbuf, struct slist *slist, struct blist *blist, int sigs_end, int *blk_requests_end, struct dpth *dpth, struct conf *conf)
{
	static char req[32]="";
	struct sbuf *sb=slist->blks_to_request;

	while(sb && !(sb->flags & SBUF_NEED_DATA)) sb=sb->next;

	if(!sb)
	{
		slist->blks_to_request=NULL;
		if(sigs_end && !*blk_requests_end)
		{
			iobuf_from_str(wbuf,
				CMD_GEN, (char *)"blk_requests_end");
			*blk_requests_end=1;
		}
		return 0;
	}
	if(!sb->burp2->bsighead)
	{
		// Trying to move onto the next file.
		// ??? Does this really work?
		if(sb->burp2->bend)
		{
			slist->blks_to_request=sb->next;
			printf("move to next\n");
		}
		if(sigs_end && !*blk_requests_end)
		{
			iobuf_from_str(wbuf,
				CMD_GEN, (char *)"blk_requests_end");
			*blk_requests_end=1;
		}
		return 0;
	}

	if(sb->burp2->bsighead->got==BLK_INCOMING)
	{
//		if(sigs_end
//		  && deduplicate(sb->burp2->bsighead, dpth, conf, wrap_up))
//			return -1;
		return 0;
	}

	if(sb->burp2->bsighead->got==BLK_NOT_GOT)
	{
		encode_req(sb->burp2->bsighead, req);
		iobuf_from_str(wbuf, CMD_DATA_REQ, req);
		sb->burp2->bsighead->requested=1;
	}

	// Move on.
	if(sb->burp2->bsighead==sb->burp2->bend)
	{
		slist->blks_to_request=sb->next;
		sb->burp2->bsighead=sb->burp2->bstart;
	}
	else
	{
		sb->burp2->bsighead=sb->burp2->bsighead->next;
	}
	return 0;
}

static void get_wbuf_from_files(struct iobuf *wbuf, struct slist *slist, struct manio *p1manio, int *requests_end)
{
	static uint64_t file_no=1;
	struct sbuf *sb=slist->last_requested;
	if(!sb)
	{
		if(manio_closed(p1manio) && !*requests_end)
		{
			iobuf_from_str(wbuf, CMD_GEN, (char *)"requests_end");
			*requests_end=1;
		}
		return;
	}

	if(sb->flags & SBUF_SENT_PATH || !(sb->flags & SBUF_NEED_DATA))
	{
		slist->last_requested=sb->next;
		return;
	}

	// Only need to request the path at this stage.
	iobuf_copy(wbuf, &sb->path);
	sb->flags |= SBUF_SENT_PATH;
	sb->burp2->index=file_no++;
}

static void sanity_before_sbuf_free(struct slist *slist, struct sbuf *sb)
{
	// It is possible for the markers to drop behind.
	if(slist->tail==sb) slist->tail=sb->next;
	if(slist->last_requested==sb) slist->last_requested=sb->next;
	if(slist->add_sigs_here==sb) slist->add_sigs_here=sb->next;
	if(slist->blks_to_request==sb) slist->blks_to_request=sb->next;
}

static int write_to_changed_file(struct asfd *chfd, struct manio *chmanio,
	struct slist *slist, struct blist *blist,
	struct dpth *dpth, int backup_end, struct conf *conf)
{
	struct sbuf *sb;
	static struct iobuf *wbuf=NULL;
	if(!slist) return 0;
        if(!wbuf && !(wbuf=iobuf_alloc())) return -1;

	while((sb=slist->head))
	{
		if(sb->flags & SBUF_NEED_DATA)
		{
			int hack=0;
			// Need data...
			struct blk *blk;

			if(!(sb->flags & SBUF_HEADER_WRITTEN_TO_MANIFEST))
			{
				if(manio_write_sbuf(chmanio, sb)) return -1;
				sb->flags |= SBUF_HEADER_WRITTEN_TO_MANIFEST;
			}

			while((blk=sb->burp2->bstart)
				&& blk->got==BLK_GOT
				&& (blk->next || backup_end))
			{
				if(*(blk->save_path))
				{
					if(manio_write_sig_and_path(chmanio,
						blk)) return -1;
					if(chmanio->sig_count==0)
					{
						// Have finished a manifest
						// file. Want to start using
						// it as a dedup candidate
						// now.
						iobuf_from_str(wbuf,
							CMD_MANIFEST,
							chmanio->fpath);
						printf("send manifest path\n");
						if(chfd->write(chfd, wbuf))
                					return -1;
					}
				}
/*
				else
				{
					// This gets hit if there is a zero
					// length file.
					printf("!!!!!!!!!!!!! no data; %s\n",
						sb->path);
					exit(1);
				}
*/

				if(blk==sb->burp2->bend)
				{
					slist->head=sb->next;
					if(!(blist->head=sb->burp2->bstart))
						blist->tail=NULL;
					sanity_before_sbuf_free(slist, sb);
					sbuf_free(&sb);
					hack=1;
					break;
				}

				if(sb->burp2->bsighead==sb->burp2->bstart)
					sb->burp2->bsighead=blk->next;
				sb->burp2->bstart=blk->next;
				if(blk==blist->blk_from_champ_chooser)
					blist->blk_from_champ_chooser=blk->next;

				//printf("freeing blk %d\n", blk->index);
				blk_free(&blk);
			}
			if(hack) continue;
			if(!(blist->head=sb->burp2->bstart))
				blist->tail=NULL;
			break;
		}
		else
		{
			// No change, can go straight in.
			if(manio_write_sbuf(chmanio, sb)) return -1;

			// Move along.
			slist->head=sb->next;

			sanity_before_sbuf_free(slist, sb);
			sbuf_free(&sb);
		}
	}
	return 0;
}

static void get_wbuf_from_wrap_up(struct iobuf *wbuf, uint64_t *wrap_up)
{
	static char *p;
	static char tmp[32];
	if(!*wrap_up) return;
//printf("get_wbuf_from_wrap_up: %d\n", *wrap_up);
	p=tmp;
	p+=to_base64(*wrap_up, tmp);
	*p='\0';
	iobuf_from_str(wbuf, CMD_WRAP_UP, tmp);
	*wrap_up=0;
}

/*
static void dump_slist(struct slist *slist, const char *msg)
{
	struct sbuf *sb;
	printf("%s\n", msg);
	for(sb=slist->head; sb; sb=sb->next)
		printf("%s\n", sb->path);
}
*/

static int maybe_add_from_scan(struct asfd *asfd,
	struct manio *p1manio, struct manio *cmanio,
	struct manio *unmanio, struct slist *slist, struct conf *conf)
{
	int ret=-1;
	static int ars;
	static int ec=0;
	struct sbuf *snew=NULL;

	while(1)
	{
		if(manio_closed(p1manio)) return 0;
		// Limit the amount loaded into memory at any one time.
		if(slist && slist->head)
		{
			if(slist->head->burp2->index
			  - slist->tail->burp2->index>4096)
				return 0;
		}
		if(!(snew=sbuf_alloc(conf))) goto end;

		if((ars=manio_sbuf_fill(p1manio,
			asfd, snew, NULL, NULL, conf))<0) goto end;
		else if(ars>0) return 0; // Finished.

		if(!(ec=entry_changed(asfd, snew, cmanio, unmanio, conf)))
		{
			// No change, no need to add to slist.
			continue;
		}
		else if(ec<0) goto end; // Error.

		if(data_needed(snew)) snew->flags|=SBUF_NEED_DATA;

		slist_add_sbuf(slist, snew);
	}
	return 0;
end:
	sbuf_free(&snew);
	return ret;
}

static int append_for_champ_chooser(struct asfd *chfd,
	struct blist *blist, int sigs_end)
{
	static int finished_sending=0;
	static struct iobuf *wbuf=NULL;
	if(!wbuf)
	{
		if(!(wbuf=iobuf_alloc())
		  || !(wbuf->buf=(char *)malloc_w(128, __func__)))
			return -1;
		wbuf->cmd=CMD_SIG;
	}
	while(blist->blk_for_champ_chooser)
	{
		// FIX THIS: This should not need to be done quite like this.
		wbuf->len=snprintf(wbuf->buf, 128, "%s%s",
			blist->blk_for_champ_chooser->weak,
			blist->blk_for_champ_chooser->strong);
		if(chfd->append_all_to_write_buffer(chfd, wbuf))
			return 0; // Try again later.
		blist->blk_for_champ_chooser=blist->blk_for_champ_chooser->next;
	}
	if(sigs_end && !finished_sending && !blist->blk_for_champ_chooser)
	{
		wbuf->cmd=CMD_GEN;
		wbuf->len=snprintf(wbuf->buf, 128, "%s", "sigs_end");
		if(chfd->append_all_to_write_buffer(chfd, wbuf))
			return 0; // Try again later.
		finished_sending++;
	}
	return 0;
}

static int mark_up_to_index(struct blist *blist,
	uint64_t index, struct dpth *dpth)
{
	struct blk *blk;
	const char *path;
//printf("in mark up\n");
//if(blist && blist->blk_from_champ_chooser) printf("YES: %p %d\n", blist->blk_from_champ_chooser, blist->blk_from_champ_chooser->index);
	// Mark everything that was not got, up to the given index.
	for(blk=blist->blk_from_champ_chooser;
	  blk && blk->index!=index; blk=blk->next)
	{
		if(blk->got!=BLK_INCOMING) continue;
		blk->got=BLK_NOT_GOT;

        	// Need to get the data for this blk from the client.
		// Set up the details of where it will be saved.
		if(!(path=dpth_mk(dpth))) return -1;
		snprintf(blk->save_path, sizeof(blk->save_path), "%s", path);
		if(dpth_incr_sig(dpth)) return -1;
	}
	if(!blk)
	{
		logp("Could not find index from champ chooser: %lu\n", index);
		return -1;
	}
	blist->blk_from_champ_chooser=blk;
	return 0;
}

static int deal_with_read_from_chfd(struct asfd *chfd,
	struct blist *blist, uint64_t *wrap_up, struct dpth *dpth)
{
	int ret=-1;
	uint64_t file_no;
	char *save_path;

	// Deal with champ chooser read here.
	//printf("read from cc: %s\n", chfd->rbuf->buf);
	switch(chfd->rbuf->cmd)
	{
		case CMD_SIG:
			// Get these for blks that the champ chooser has found.
			file_no=decode_file_no_and_save_path(chfd->rbuf,
				&save_path);
			//printf("got save_path: %d %s\n", file_no, save_path);
			if(mark_up_to_index(blist, file_no, dpth)) goto end;
			// FIX THIS:
			// blist->blk_from_champ_chooser needs to have the
			// save path set.
			blist->blk_from_champ_chooser->got=BLK_GOT;
			snprintf(blist->blk_from_champ_chooser->save_path,
			  sizeof(blist->blk_from_champ_chooser->save_path),
				"%s", save_path);
			//printf("after cmd_sig: %d\n",
			//	blist->blk_from_champ_chooser->index);
			break;
		case CMD_WRAP_UP:
			//*wrap_up=decode_file_no(chfd->rbuf);
			file_no=decode_file_no(chfd->rbuf);
			//printf("mark up to: %d\n", file_no);
			if(mark_up_to_index(blist, file_no, dpth)) goto end;
			//printf("after mark_up: %d\n",
			//	blist->blk_from_champ_chooser->index);
			break;
		default:
			iobuf_log_unexpected(chfd->rbuf, __func__);
			goto end;
	}
	ret=0;
end:
	iobuf_free_content(chfd->rbuf);
	return ret;
}

int backup_phase2_server(struct async *as, struct sdirs *sdirs,
	const char *manifest_dir, int resume, struct conf *conf)
{
	int ret=-1;
	int sigs_end=0;
	int backup_end=0;
	int requests_end=0;
	int blk_requests_end=0;
	struct slist *slist=NULL;
	struct blist *blist=NULL;
	struct iobuf *wbuf=NULL;
	struct dpth *dpth=NULL;
	struct manio *cmanio=NULL;	// current manifest
	struct manio *p1manio=NULL;	// phase1 scan manifest
	struct manio *chmanio=NULL;	// changed manifest
	struct manio *unmanio=NULL;	// unchanged manifest
	// This is used to tell the client that a number of consecutive blocks
	// have been found and can be freed.
	uint64_t wrap_up=0;
	// Main fd is first in the list.
	struct asfd *asfd=as->asfd;
	// Champ chooser fd is second in the list.
	struct asfd *chfd=asfd->next;

	logp("Phase 2 begin (recv backup data)\n");

	if(champ_chooser_init(sdirs->data, conf)
	  || !(cmanio=manio_alloc())
	  || !(p1manio=manio_alloc())
	  || !(chmanio=manio_alloc())
	  || !(unmanio=manio_alloc())
	  || manio_init_read(cmanio, sdirs->cmanifest)
	  || manio_init_read(p1manio, sdirs->phase1data)
	  || manio_init_write(chmanio, sdirs->changed)
	  || manio_init_write(unmanio, sdirs->unchanged)
	  || !(slist=slist_alloc())
	  || !(blist=blist_alloc())
	  || !(wbuf=iobuf_alloc())
	  || !(dpth=dpth_alloc(sdirs->data))
	  || dpth_init(dpth))
		goto end;

	// The phase1 manifest looks the same as a burp1 one.
	manio_set_protocol(p1manio, PROTO_BURP1);

	while(!backup_end)
	{
		if(maybe_add_from_scan(asfd,
			p1manio, cmanio, unmanio, slist, conf))
				goto end;

		if(!wbuf->len)
		{
			get_wbuf_from_wrap_up(wbuf, &wrap_up);
			if(!wbuf->len)
			{
				if(get_wbuf_from_sigs(wbuf, slist, blist,
				  sigs_end, &blk_requests_end, dpth, conf))
					goto end;
				if(!wbuf->len)
				{
					get_wbuf_from_files(wbuf, slist,
						p1manio, &requests_end);
				}
			}
		}

		if(wbuf->len)
			asfd->append_all_to_write_buffer(asfd, wbuf);

		append_for_champ_chooser(chfd, blist, sigs_end);

		if(as->read_write(as))
		{
			logp("error in %s\n", __func__);
			goto end;
		}

		while(asfd->rbuf->buf)
		{
			if(deal_with_read(asfd->rbuf, slist, blist,
				conf, &sigs_end, &backup_end, dpth))
					goto end;
			// Get as much out of the
			// readbuf as possible.
			if(asfd->parse_readbuf(asfd)) goto end;
		}
		while(chfd->rbuf->buf)
		{
			if(deal_with_read_from_chfd(chfd,
				blist, &wrap_up, dpth)) goto end;
			// Get as much out of the
			// readbuf as possible.
			if(chfd->parse_readbuf(chfd)) goto end;
		}

		if(write_to_changed_file(chfd, chmanio,
			slist, blist, dpth, backup_end, conf))
				goto end;
	}

	// Hack: If there are some entries left after the last entry that
	// contains block data, it will not be written to the changed file
	// yet because the last entry of block data has not had
	// sb->burp2->bend set.
	if(slist->head && slist->head->next)
	{
		slist->head=slist->head->next;
		if(write_to_changed_file(chfd, chmanio,
			slist, blist, dpth, backup_end, conf))
				goto end;
	}

	if(manio_close(unmanio)
	  || manio_close(chmanio))
		goto end;

	if(blist->head)
	{
		logp("ERROR: finishing but still want block: %lu\n",
			blist->head->index);
		goto end;
	}

	// Need to release the last left. There should be one at most.
	if(dpth->head && dpth->head->next)
	{
		logp("ERROR: More data locks remaining after: %s\n",
			dpth->head->save_path);
		goto end;
	}
	if(dpth_release_all(dpth)) goto end;

	ret=0;
end:
	logp("End backup\n");
	slist_free(slist);
	blist_free(blist);
	iobuf_free_content(asfd->rbuf);
	iobuf_free_content(chfd->rbuf);
	// Write buffer did not allocate 'buf'. 
	if(wbuf) wbuf->buf=NULL;
	iobuf_free(wbuf);
	dpth_release_all(dpth);
	dpth_free(&dpth);
	manio_free(&cmanio);
	manio_free(&p1manio);
	manio_free(&chmanio);
	manio_free(&unmanio);
	return ret;
}