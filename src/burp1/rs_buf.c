/*= -*- c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *
 * librsync -- the library for network deltas
 * $Id: buf.c,v 1.22 2003/12/16 00:10:55 abo Exp $
 * 
 * Copyright (C) 2000, 2001 by Martin Pool <mbp@samba.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

                              /*
                               | Pick a window, Jimmy, you're leaving.
                               |   -- Martin Schwenke, regularly
                               */


/*
 * buf.c -- Buffers that map between stdio file streams and librsync
 * streams.  As the stream consumes input and produces output, it is
 * refilled from appropriate input and output FILEs.  A dynamically
 * allocated buffer of configurable size is used as an intermediary.
 */

#include "include.h"

/* use fseeko instead of fseek for long file support if we have it */
#ifdef HAVE_FSEEKO
#define fseek fseeko
#endif

void *rs_alloc(size_t size)
{
	return calloc_w(1, size, __func__);
}

rs_filebuf_t *rs_filebuf_new(struct asfd *asfd,
	BFILE *bfd, FILE *fp, gzFile zp, int fd,
	size_t buf_len, size_t data_len, struct cntr *cntr)
{
	rs_filebuf_t *pf=NULL;
	if(!(pf=(struct rs_filebuf *)calloc_w(1,
		sizeof(struct rs_filebuf), __func__))) return NULL;

	if(!(pf->buf=(char *)calloc_w(1, buf_len, __func__)))
	{
		free(pf);
		return NULL;
	}
	pf->buf_len=buf_len;
	pf->fp=fp;
	pf->zp=zp;
	pf->fd=fd;
	pf->bfd=bfd;
	pf->bytes=0;
	pf->data_len=data_len;
	if(data_len>0)
		pf->do_known_byte_count=1;
	else
		pf->do_known_byte_count=0;
	pf->cntr=cntr;
	if(!MD5_Init(&(pf->md5)))
	{
		logp("MD5_Init() failed\n");
		rs_filebuf_free(pf);
		return NULL;
	}
	pf->asfd=asfd;

	return pf;
}

void rs_filebuf_free(rs_filebuf_t *fb) 
{
	if(fb->buf) free(fb->buf);
	memset(fb, 0, sizeof(*fb));
        free(fb);
	fb=NULL;
}

/*
 * If the stream has no more data available, read some from F into
 * BUF, and let the stream use that.  On return, SEEN_EOF is true if
 * the end of file has passed into the stream.
 */
rs_result rs_infilebuf_fill(rs_job_t *job, rs_buffers_t *buf, void *opaque)
{
	int                     len=0;
	rs_filebuf_t            *fb = (rs_filebuf_t *) opaque;
	gzFile                  zp = fb->zp;
	FILE                    *fp = fb->fp;
	struct cntr *cntr;
	int fd=fb->fd;
	cntr=fb->cntr;

	//logp("rs_infilebuf_fill\n");

	/* This is only allowed if either the buf has no input buffer
	 * yet, or that buffer could possibly be BUF. */
	if(buf->next_in)
	{
		//logp("infilebuf avail_in %d buf_len %d\n",
		//	buf->avail_in, fb->buf_len);
		if(buf->avail_in > fb->buf_len)
		{
			logp("buf->avail_in > fb->buf_len (%d > %d) in %s\n",
				buf->avail_in, fb->buf_len, __func__);
			return RS_IO_ERROR;
		}
		if(buf->next_in < fb->buf)
		{
			logp("buf->next_in < fb->buf in %s\n", __func__);
			return RS_IO_ERROR;
		}
		if(buf->next_in > fb->buf + fb->buf_len)
		{
			logp("buf->next_in > fb->buf + fb->buf_len in %s\n",
				__func__);
			return RS_IO_ERROR;
		}
	}
	else
	{
		if(buf->avail_in)
		{
			logp("buf->avail_in is %d in %s\n",
				buf->avail_in, __func__);
			return RS_IO_ERROR;
		}
	}

	if(buf->eof_in) return RS_DONE;

	if(buf->avail_in)
		/* Still some data remaining.  Perhaps we should read
		   anyhow? */
		return RS_DONE;

	if(fd>=0)
	{
		static struct iobuf *rbuf=NULL;
		rbuf=fb->asfd->rbuf;

		if(fb->asfd->read(fb->asfd)) return RS_IO_ERROR;
		if(rbuf->cmd==CMD_APPEND)
		{
			//logp("got '%c' in fd infilebuf: %d\n",
			//	CMD_APPEND, rbuf->len);
			memcpy(fb->buf, rbuf->buf, rbuf->len);
			len=rbuf->len;
			iobuf_free_content(rbuf);
		}
		else if(rbuf->cmd==CMD_END_FILE)
		{
			iobuf_free_content(rbuf);
			//logp("got %c in fd infilebuf\n", CMD_END_FILE);
			buf->eof_in=1;
			return RS_DONE;
		}
		else if(rbuf->cmd==CMD_WARNING)
		{
			logp("WARNING: %s\n", rbuf->buf);
			cntr_add(cntr, rbuf->cmd, 0);
			iobuf_free_content(rbuf);
			return RS_DONE;
		}
		else
		{
			iobuf_log_unexpected(rbuf, __func__);
			iobuf_free_content(rbuf);
			return RS_IO_ERROR;
		}
	}
	else if(fb->bfd)
	{
		if(fb->do_known_byte_count)
		{
			if(fb->data_len>0)
			{
				len=fb->bfd->read(fb->bfd, fb->buf,
					min(fb->buf_len, fb->data_len));
				fb->data_len-=len;
			}
			else
			{
				// We have already read as much data as the VSS
				// header told us to, so set len=0 in order to
				// finish up.
				len=0;
			}
		}
		else
			len=fb->bfd->read(fb->bfd, fb->buf, fb->buf_len);
		if(len==0)
		{
			//logp("bread: eof\n");
			buf->eof_in=1;
			return RS_DONE;
		}
		else if(len<0)
		{
			logp("rs_infilebuf_fill: error in bread\n");
			return RS_IO_ERROR;
		}
		//logp("bread: ok: %d\n", len);
		fb->bytes+=len;
		if(!MD5_Update(&(fb->md5), fb->buf, len))
		{
			logp("rs_infilebuf_fill: MD5_Update() failed\n");
			return RS_IO_ERROR;
		}
	}
	else if(fp)
	{
		len = fread(fb->buf, 1, fb->buf_len, fp);
		//logp("fread: %d\n", len);
		if(len <= 0)
		{
			/* This will happen if file size is a multiple of
			   input block len
			 */
			if(feof(fp))
			{
				buf->eof_in=1;
				return RS_DONE;
			}
			else
			{
				logp("rs_infilebuf_fill: got return %d when trying to read\n", len);
				return RS_IO_ERROR;
			}
		}
		fb->bytes+=len;
		if(!MD5_Update(&(fb->md5), fb->buf, len))
		{
			logp("rs_infilebuf_fill: MD5_Update() failed\n");
			return RS_IO_ERROR;
		}
	}
	else if(zp)
	{
		len=gzread(zp, fb->buf, fb->buf_len);
		//logp("gzread: %d\n", len);
		if(len <= 0)
		{
			/* This will happen if file size is a multiple of input block len
			 */
			if(gzeof(zp))
			{
				buf->eof_in=1;
				return RS_DONE;
			}
			else
			{
				logp("rs_infilebuf_fill: got return %d when trying to read\n", len);
				return RS_IO_ERROR;
			}
		}
		fb->bytes+=len;
		if(!MD5_Update(&(fb->md5), fb->buf, len))
		{
			logp("rs_infilebuf_fill: MD5_Update() failed\n");
			return RS_IO_ERROR;
		}
	}

	buf->avail_in = len;
	buf->next_in = fb->buf;

	return RS_DONE;
}

/*
 * The buf is already using BUF for an output buffer, and probably
 * contains some buffered output now.  Write this out to F, and reset
 * the buffer cursor.
 */
rs_result rs_outfilebuf_drain(rs_job_t *job, rs_buffers_t *buf, void *opaque)
{
	rs_filebuf_t *fb = (rs_filebuf_t *) opaque;
	FILE *fp = fb->fp;
	gzFile zp = fb->zp;
	int fd = fb->fd;
	size_t wlen;

	//logp("in rs_outfilebuf_drain\n");

	/* This is only allowed if either the buf has no output buffer
	 * yet, or that buffer could possibly be BUF. */
	if(!buf->next_out)
	{
		if(buf->avail_out)
		{
			logp("buf->avail_out is %d in %s\n",
				buf->avail_out, __func__);
			return RS_IO_ERROR;
		}
		buf->next_out = fb->buf;
		buf->avail_out = fb->buf_len;
		return RS_DONE;
	}

	if(buf->avail_out > fb->buf_len)
	{
		logp("buf->avail_out > fb->buf_len (%d > %d) in %s\n",
			buf->avail_out, fb->buf_len, __func__);
		return RS_IO_ERROR;
	}
	if(buf->next_out < fb->buf)
	{
		logp("buf->next_out < fb->buf (%p < %p) in %s\n",
			buf->next_out, fb->buf, __func__);
		return RS_IO_ERROR;
	}
	if(buf->next_out > fb->buf + fb->buf_len)
	{
		logp("buf->next_out > fb->buf + fb->buf_len in %s\n",
			__func__);
		return RS_IO_ERROR;
	}

	if((wlen=buf->next_out-fb->buf)>0)
	{
		//logp("wlen: %d\n", wlen);
		if(fd>0)
		{
			size_t w=wlen;
			static struct iobuf *wbuf=NULL;
			if(!wbuf && !(wbuf=iobuf_alloc())) return RS_IO_ERROR;
			wbuf->cmd=CMD_APPEND;
			wbuf->buf=fb->buf;
			wbuf->len=wlen;
			switch(fb->asfd->append_all_to_write_buffer(
				fb->asfd, wbuf))
			{
				case APPEND_OK: break;
				case APPEND_BLOCKED: return RS_BLOCKED;
				case APPEND_ERROR:
				default: return RS_IO_ERROR;
			}
			fb->bytes+=w;
		}
		else
		{
			size_t result=0;
			if(fp) result=fwrite(fb->buf, 1, wlen, fp);
			else if(zp) result=gzwrite(zp, fb->buf, wlen);
			if(wlen!=result)
			{
				logp("error draining buf to file: %s",
						strerror(errno));
				return RS_IO_ERROR;
			}
		}
	}

	buf->next_out = fb->buf;
	buf->avail_out = fb->buf_len;

	return RS_DONE;
}

rs_result do_rs_run(struct asfd *asfd,
	rs_job_t *job, BFILE *bfd,
	FILE *in_file, FILE *out_file,
	gzFile in_zfile, gzFile out_zfile, int infd, int outfd, struct cntr *cntr)
{
	rs_buffers_t buf;
	rs_result result;
	rs_filebuf_t *in_fb=NULL;
	rs_filebuf_t *out_fb=NULL;

	if(in_file && infd>=0)
	{
		logp("do not specify both input file and input fd in do_rs_run()\n");
		return RS_IO_ERROR;
	}
	if(out_file && outfd>=0)
	{
		logp("do not specify both output file and output fd in do_rs_run()\n");
		return RS_IO_ERROR;
	}

	if((bfd || in_file || in_zfile || infd>=0)
	 && !(in_fb=rs_filebuf_new(asfd, bfd,
		in_file, in_zfile, infd, ASYNC_BUF_LEN, -1, cntr)))
			return RS_MEM_ERROR;
	if((out_file || out_zfile || outfd>=0)
	 && !(out_fb=rs_filebuf_new(asfd, NULL,
		out_file, out_zfile, outfd, ASYNC_BUF_LEN, -1, cntr)))
	{
		if(in_fb) rs_filebuf_free(in_fb);
		return RS_MEM_ERROR;
	}

//logp("before rs_job_drive\n");
	result = rs_job_drive(job, &buf,
		in_fb ? rs_infilebuf_fill : NULL, in_fb,
		out_fb ? rs_outfilebuf_drain : NULL, out_fb);
//logp("after rs_job_drive\n");

	if(in_fb) rs_filebuf_free(in_fb);
	if(out_fb) rs_filebuf_free(out_fb);

	return result;
}

static rs_result rs_async_drive(rs_job_t *job, rs_buffers_t *rsbuf,
             rs_driven_cb in_cb, void *in_opaque,
             rs_driven_cb out_cb, void *out_opaque)
{
	rs_result result;
	rs_result iores;

	if (!rsbuf->eof_in && in_cb)
	{
		iores = in_cb(job, rsbuf, in_opaque);
		if (iores != RS_DONE) return iores;
	}

	result = rs_job_iter(job, rsbuf);
	if (result != RS_DONE && result != RS_BLOCKED)
		return result;
	//printf("job iter got: %d\n", result);

	if (out_cb)
	{
		iores = (out_cb)(job, rsbuf, out_opaque);
		if (iores != RS_DONE) return iores;
	}

	return result;
}

rs_result rs_async(rs_job_t *job, rs_buffers_t *rsbuf,
	rs_filebuf_t *infb, rs_filebuf_t *outfb)
{
	return rs_async_drive(job, rsbuf,
		infb ? rs_infilebuf_fill : NULL, infb,
		outfb ? rs_outfilebuf_drain : NULL, outfb);
}

static rs_result rs_whole_gzrun(struct asfd *asfd,
	rs_job_t *job, FILE *in_file, gzFile in_zfile,
	FILE *out_file, gzFile out_zfile, struct cntr *cntr)
{
	rs_buffers_t buf;
	rs_result result;
	rs_filebuf_t *in_fb=NULL;
	rs_filebuf_t *out_fb=NULL;

	if(in_file || in_zfile)
		in_fb=rs_filebuf_new(asfd, NULL,
			in_file, in_zfile, -1, ASYNC_BUF_LEN, -1, cntr);

	if(out_file || out_zfile)
	out_fb=rs_filebuf_new(asfd, NULL,
		out_file, out_zfile, -1, ASYNC_BUF_LEN, -1, cntr);
	result=rs_job_drive(job, &buf,
		in_fb ? rs_infilebuf_fill : NULL, in_fb,
		out_fb ? rs_outfilebuf_drain : NULL, out_fb);

	if(in_fb) rs_filebuf_free(in_fb);
	if(out_fb) rs_filebuf_free(out_fb);

	return result;
}

rs_result rs_patch_gzfile(struct asfd *asfd, FILE *basis_file, FILE *delta_file,
	gzFile delta_zfile, FILE *new_file, gzFile new_zfile,
	rs_stats_t *stats, struct cntr *cntr)
{
	rs_job_t *job;
	rs_result r;

	job=rs_patch_begin(rs_file_copy_cb, basis_file);
	r=rs_whole_gzrun(asfd, job,
		delta_file, delta_zfile, new_file, new_zfile, cntr);
	rs_job_free(job);

	return r;
}

rs_result rs_sig_gzfile(struct asfd *asfd,
	FILE *old_file, gzFile old_zfile, FILE *sig_file,
	size_t new_block_len, size_t strong_len,
	rs_stats_t *stats, struct cntr *cntr)
{
	rs_job_t *job;
	rs_result r;

	job=rs_sig_begin(new_block_len, strong_len);
	r=rs_whole_gzrun(asfd, job, old_file, old_zfile, sig_file, NULL, cntr);
	rs_job_free(job);

	return r;
}

rs_result rs_delta_gzfile(struct asfd *asfd,
	rs_signature_t *sig, FILE *new_file,
	gzFile new_zfile, FILE *delta_file, gzFile delta_zfile,
	rs_stats_t *stats, struct cntr *cntr)
{
	rs_job_t *job;
	rs_result r;

	job=rs_delta_begin(sig);
	r=rs_whole_gzrun(asfd, job,
		new_file, new_zfile, delta_file, delta_zfile, cntr);
	rs_job_free(job);

	return r;
}
