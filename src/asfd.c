#include "include.h"

// For IPTOS / IPTOS_THROUGHPUT.
#ifdef HAVE_WIN32
#include <ws2tcpip.h>
#else
#include <netinet/ip.h>
#endif

#ifdef HAVE_NCURSES_H
#include "ncurses.h"
#endif

#include "burp2/blist.h"

static size_t bufmaxsize=(ASYNC_BUF_LEN*2)+32;

static void truncate_readbuf(struct asfd *asfd)
{
	asfd->readbuf[0]='\0';
	asfd->readbuflen=0;
}

static int asfd_alloc_buf(char **buf)
{
	if(!*buf && !(*buf=(char *)calloc_w(1, bufmaxsize, __func__)))
		return -1;
	return 0;
}

static int extract_buf(struct asfd *asfd,
	unsigned int len, unsigned int offset)
{
	if(!(asfd->rbuf->buf=(char *)malloc_w(len+1, __func__)))
		return -1;
	if(!(memcpy(asfd->rbuf->buf, asfd->readbuf+offset, len)))
	{
		logp("%s: memcpy failed in %s\n", asfd->desc, __func__);
		return -1;
	}
	asfd->rbuf->buf[len]='\0';
	if(!(memmove(asfd->readbuf,
		asfd->readbuf+len+offset, asfd->readbuflen-len-offset)))
	{
		logp("%s: memmove failed in %s\n", asfd->desc, __func__);
		return -1;
	}
	asfd->readbuflen-=len+offset;
	asfd->rbuf->len=len;
	return 0;
}

#ifdef HAVE_NCURSES_H
static int parse_readbuf_ncurses(struct asfd *asfd)
{
	if(!asfd->readbuflen) return 0;
	// This is reading ints, and will be cast back to an int when it comes
	// to be processed later.
	if(extract_buf(asfd, asfd->readbuflen, 0)) return -1;
	return 0;
}
#endif

static int parse_readbuf_line_buf(struct asfd *asfd)
{
	static char *cp=NULL;
	static char *dp=NULL;
	static size_t len=0;
	if(!cp)
	{
		// Only start from the beginning if we previously got something
		// to extract.
		cp=asfd->readbuf;
		len=0;
	}
	for(; len<asfd->readbuflen; cp++, len++)
	{
		if(*cp!='\n') continue;
		len++;
		if(extract_buf(asfd, len, 0)) return -1;
		// Strip trailing white space, like '\r\n'.
		dp=asfd->rbuf->buf;
		for(cp=&(dp[len-1]); cp>=dp && isspace(*cp); cp--, len--)
			*cp='\0';
		asfd->rbuf->len=len;
		break;
	}
	cp=NULL;
	return 0;
}

static int parse_readbuf_standard(struct asfd *asfd)
{
	char cmdtmp='\0';
	unsigned int s=0;
	if(asfd->readbuflen<5) return 0;
	if((sscanf(asfd->readbuf, "%c%04X", &cmdtmp, &s))!=2)
	{
		logp("%s: sscanf of '%s' failed in %s\n",
			asfd->desc, asfd->readbuf, __func__);
		return -1;
	}
	if(asfd->readbuflen>=s+5)
	{
		asfd->rbuf->cmd=cmdtmp;
		if(extract_buf(asfd, s, 5))
			return -1;
	}
	return 0;
}

static int asfd_parse_readbuf(struct asfd *asfd)
{
	if(asfd->rbuf->buf) return 0;

	if(asfd->parse_readbuf_specific(asfd))
	{
		truncate_readbuf(asfd);
		return -1;
	}

	return 0;
}

#ifdef HAVE_NCURSES_H
static int asfd_do_read_ncurses(struct asfd *asfd)
{
	static int i;
	i=getch();
	asfd->readbuflen=sizeof(int);
	memcpy(asfd->readbuf, &i, asfd->readbuflen);
	return 0;
}

static int asfd_do_write_ncurses(struct asfd *asfd)
{
	logp("This function should not have been called: %s\n", __func__);
	return -1;
}
#endif

static int asfd_do_read(struct asfd *asfd)
{
	ssize_t r;
	r=read(asfd->fd,
		asfd->readbuf+asfd->readbuflen, bufmaxsize-asfd->readbuflen);
	if(r<0)
	{
		if(errno==EAGAIN || errno==EINTR)
			return 0;
		logp("%s: read problem on fd %d: %s\n",
			asfd->desc, asfd->fd, strerror(errno));
		goto error;
	}
	else if(!r)
	{
		// End of data.
		logp("%s: end of data\n", asfd->desc);
		goto error;
	}
	asfd->readbuflen+=r;
	asfd->readbuf[asfd->readbuflen]='\0';
	return 0;
error:
	truncate_readbuf(asfd);
	return -1;
}

static int asfd_do_read_ssl(struct asfd *asfd)
{
	int e;
	ssize_t r;

	asfd->read_blocked_on_write=0;

	ERR_clear_error();
	r=SSL_read(asfd->ssl,
		asfd->readbuf+asfd->readbuflen, bufmaxsize-asfd->readbuflen);

	switch((e=SSL_get_error(asfd->ssl, r)))
	{
		case SSL_ERROR_NONE:
			asfd->readbuflen+=r;
			asfd->readbuf[asfd->readbuflen]='\0';
			break;
		case SSL_ERROR_ZERO_RETURN:
			// End of data.
			logp("%s: Peer closed SSL session\n", asfd->desc);
			SSL_shutdown(asfd->ssl);
			goto error;
		case SSL_ERROR_WANT_READ:
			break;
		case SSL_ERROR_WANT_WRITE:
			asfd->read_blocked_on_write=1;
			break;
		case SSL_ERROR_SYSCALL:
			if(errno==EAGAIN || errno==EINTR)
				break;
			logp("%s: Got SSL_ERROR_SYSCALL\n",
				asfd->desc);
			// Fall through to read problem
		default:
			logp_ssl_err(
				"%s: SSL read problem in %s: %d - %d=%s\n",
				asfd->desc, __func__,
				e, errno, strerror(errno));
			goto error;
	}
	return 0;
error:
	truncate_readbuf(asfd);
	return -1;
}

// Return 0 for OK to write, non-zero for not OK to write.
static int check_ratelimit(struct asfd *asfd)
{
	float f;
	time_t diff;
	if(!asfd->rlstart) asfd->rlstart=time(NULL);
	if((diff=asfd->as->now-asfd->rlstart)<0)
	{
		// It is possible that the clock changed. Reset ourselves.
		asfd->as->now=asfd->rlstart;
		asfd->rlbytes=0;
		logp("Looks like the clock went back in time since starting. "
			"Resetting ratelimit\n");
		return 0;
	}
	if(!diff) return 0; // Need to get started somehow.
	f=(asfd->rlbytes)/diff; // Bytes per second.

	if(f>=asfd->ratelimit)
	{
#ifdef HAVE_WIN32
		// Windows Sleep is milliseconds, usleep is microseconds.
		// Do some conversion.
		Sleep(asfd->rlsleeptime/1000);
#else
		usleep(asfd->rlsleeptime);
#endif
		// If sleeping, increase the sleep time.
		if((asfd->rlsleeptime*=2)>=500000) asfd->rlsleeptime=500000;
		return 1;
	}
	// If not sleeping, decrease the sleep time.
	if((asfd->rlsleeptime/=2)<=9999) asfd->rlsleeptime=10000;
	return 0;
}

static int asfd_do_write(struct asfd *asfd)
{
	ssize_t w;
	if(asfd->ratelimit && check_ratelimit(asfd)) return 0;

	w=write(asfd->fd, asfd->writebuf, asfd->writebuflen);
	if(w<0)
	{
		if(errno==EAGAIN || errno==EINTR)
			return 0;
		logp("%s: Got error in %s, (%d=%s)\n", __func__,
			asfd->desc, errno, strerror(errno));
		return -1;
	}
	else if(!w)
	{
		logp("%s: Wrote nothing in %s\n", asfd->desc, __func__);
		return -1;
	}
	if(asfd->ratelimit) asfd->rlbytes+=w;
/*
{
char buf[100000]="";
snprintf(buf, w+1, "%s", asfd->writebuf);
printf("wrote %d: %s\n", w, buf);
}
*/

	memmove(asfd->writebuf, asfd->writebuf+w, asfd->writebuflen-w);
	asfd->writebuflen-=w;
	return 0;
}

static int asfd_do_write_ssl(struct asfd *asfd)
{
	int e;
	ssize_t w;

	asfd->write_blocked_on_read=0;

	if(asfd->ratelimit && check_ratelimit(asfd)) return 0;
	ERR_clear_error();
	w=SSL_write(asfd->ssl, asfd->writebuf, asfd->writebuflen);

	switch((e=SSL_get_error(asfd->ssl, w)))
	{
		case SSL_ERROR_NONE:
/*
{
char buf[100000]="";
snprintf(buf, w+1, "%s", asfd->writebuf);
printf("wrote %d: %s\n", w, buf);
}
*/
			if(asfd->ratelimit) asfd->rlbytes+=w;
			memmove(asfd->writebuf,
				asfd->writebuf+w, asfd->writebuflen-w);
			asfd->writebuflen-=w;
			break;
		case SSL_ERROR_WANT_WRITE:
			break;
		case SSL_ERROR_WANT_READ:
			asfd->write_blocked_on_read=1;
			break;
		case SSL_ERROR_SYSCALL:
			if(errno==EAGAIN || errno==EINTR)
				break;
			logp("%s: Got SSL_ERROR_SYSCALL\n",
				asfd->desc);
			// Fall through to read problem
		default:
			logp_ssl_err(
				"%s: SSL write problem in %s: %d - %d=%s\n",
				asfd->desc, __func__,
				e, errno, strerror(errno));
			return -1;
	}
	return 0;
}

static int append_to_write_buffer(struct asfd *asfd,
	const char *buf, size_t len)
{
	memcpy(asfd->writebuf+asfd->writebuflen, buf, len);
	asfd->writebuflen+=len;
	asfd->writebuf[asfd->writebuflen]='\0';
	return 0;
}

static enum append_ret asfd_append_all_to_write_buffer(struct asfd *asfd,
	struct iobuf *wbuf)
{
	switch(asfd->streamtype)
	{
		case ASFD_STREAM_STANDARD:
		{
			size_t sblen=0;
			char sbuf[10]="";
			if(asfd->writebuflen+6+(wbuf->len) >= bufmaxsize-1)
				return APPEND_BLOCKED;

			snprintf(sbuf, sizeof(sbuf), "%c%04X",
				wbuf->cmd, (unsigned int)wbuf->len);
			sblen=strlen(sbuf);
			append_to_write_buffer(asfd, sbuf, sblen);
			break;
		}
		case ASFD_STREAM_LINEBUF:
			if(asfd->writebuflen+wbuf->len >= bufmaxsize-1)
				return APPEND_BLOCKED;
			break;
		case ASFD_STREAM_NCURSES_STDIN:
		default:
			logp("%s: unknown asfd stream type in %s: %d\n",
				asfd->desc, __func__, asfd->streamtype);
			return APPEND_ERROR;
	}
	append_to_write_buffer(asfd, wbuf->buf, wbuf->len);
//printf("append %d: %c:%s\n", wbuf->len, wbuf->cmd, wbuf->buf);
	wbuf->len=0;
	return APPEND_OK;
}

static int asfd_set_bulk_packets(struct asfd *asfd)
{
#ifdef IP_TOS
#ifndef IPTOS_THROUGHPUT
// Windows/mingw64 does not define this, but it is just a bit in the packet
// header. Set it ourselves. According to what I have read on forums, the
// Windows machine may have some system wide policy that resets the bits.
// At least the burp code will be doing the right thing by setting it, even
// if Windows decides to remove it.
#define IPTOS_THROUGHPUT 0x08
#endif
	int opt=IPTOS_THROUGHPUT;
	if(asfd->fd<0) return -1;
	if(setsockopt(asfd->fd,
		IPPROTO_IP, IP_TOS, (char *)&opt, sizeof(opt))<0)
	{
		logp("%s: error: setsockopt IPTOS_THROUGHPUT: %s\n",
			asfd->desc, strerror(errno));
		return -1;
	}
#endif
	return 0;
}

static int asfd_read(struct asfd *asfd)
{
	if(asfd->as->doing_estimate) return 0;
	while(!asfd->rbuf->buf)
		if(asfd->as->read_write(asfd->as)) return -1;
	return 0;
}

static int asfd_read_expect(struct asfd *asfd, char cmd, const char *expect)
{
	int ret=0;
	if(asfd->read(asfd)) return -1;
	if(asfd->rbuf->cmd!=cmd || strcmp(asfd->rbuf->buf, expect))
	{
		logp("%s: expected '%c:%s', got '%c:%s'\n",
			asfd->desc, cmd, expect,
			asfd->rbuf->cmd, asfd->rbuf->buf);
		ret=-1;
	}
	iobuf_free_content(asfd->rbuf);
	return ret;
}

static int asfd_write(struct asfd *asfd, struct iobuf *wbuf)
{
	if(asfd->as->doing_estimate) return 0;
	while(wbuf->len)
	{
		if(asfd->append_all_to_write_buffer(asfd, wbuf)==APPEND_ERROR)
			return -1;
		if(asfd->as->write(asfd->as)) return -1;
	}
	return 0;
}

static int asfd_write_strn(struct asfd *asfd,
	char wcmd, const char *wsrc, size_t len)
{
	struct iobuf wbuf;
	wbuf.cmd=wcmd;
	wbuf.buf=(char *)wsrc;
	wbuf.len=len;
	return asfd->write(asfd, &wbuf);
}

static int asfd_write_str(struct asfd *asfd, char wcmd, const char *wsrc)
{
	return asfd_write_strn(asfd, wcmd, wsrc, strlen(wsrc));
}

static int asfd_simple_loop(struct asfd *asfd,
	struct conf *conf, void *param, const char *caller,
  enum asl_ret callback(struct asfd *asfd, struct conf *conf, void *param))
{
	struct iobuf *rbuf=asfd->rbuf;
	while(1)
	{
		iobuf_free_content(rbuf);
		if(asfd->read(asfd)) goto error;
		if(rbuf->cmd!=CMD_GEN)
		{
			if(rbuf->cmd==CMD_WARNING)
			{
				logp("WARNING: %s\n", rbuf->buf);
				cntr_add(conf->cntr, rbuf->cmd, 0);
			}
			else if(rbuf->cmd==CMD_INTERRUPT)
			{
				// Ignore - client wanted to interrupt a file.
			}
			else
			{
				logp("%s: unexpected command in %s(), called from %s(): %c:%s\n", asfd->desc, __func__, caller, rbuf->cmd, rbuf->buf);
				goto error;
			}
			continue;
		}
		switch(callback(asfd, conf, param))
		{
			case ASL_CONTINUE: break;
			case ASL_END_OK:
				iobuf_free_content(rbuf);
				return 0;
			case ASL_END_OK_RETURN_1:
				iobuf_free_content(rbuf);
				return 1;
			case ASL_END_ERROR:
			default:
				goto error;
		}
	}
error:
	iobuf_free_content(rbuf);
	return -1;
}


static int asfd_init(struct asfd *asfd, const char *desc,
	struct async *as, int afd, SSL *assl,
	enum asfd_streamtype streamtype, struct conf *conf)
{
	asfd->as=as;
	asfd->fd=afd;
	asfd->ssl=assl;
	asfd->streamtype=streamtype;
	asfd->max_network_timeout=conf->network_timeout;
	asfd->network_timeout=asfd->max_network_timeout;
	asfd->ratelimit=conf->ratelimit;
	asfd->rlsleeptime=10000;
	asfd->pid=-1;

	asfd->parse_readbuf=asfd_parse_readbuf;
	asfd->append_all_to_write_buffer=asfd_append_all_to_write_buffer;
	asfd->set_bulk_packets=asfd_set_bulk_packets;
	if(asfd->ssl)
	{
		asfd->do_read=asfd_do_read_ssl;
		asfd->do_write=asfd_do_write_ssl;
	}
	else
	{
		asfd->do_read=asfd_do_read;
		asfd->do_write=asfd_do_write;
#ifdef HAVE_NCURSES_H
		if(asfd->streamtype==ASFD_STREAM_NCURSES_STDIN)
		{
			asfd->do_read=asfd_do_read_ncurses;
			asfd->do_write=asfd_do_write_ncurses;
		}
#endif
	}
	asfd->read=asfd_read;
	asfd->read_expect=asfd_read_expect;
	asfd->simple_loop=asfd_simple_loop;
	asfd->write=asfd_write;
	asfd->write_str=asfd_write_str;
	asfd->write_strn=asfd_write_strn;

	switch(asfd->streamtype)
	{
		case ASFD_STREAM_STANDARD:
			asfd->parse_readbuf_specific=parse_readbuf_standard;
			break;
		case ASFD_STREAM_LINEBUF:
			asfd->parse_readbuf_specific=parse_readbuf_line_buf;
			break;
#ifdef HAVE_NCURSES_H
		case ASFD_STREAM_NCURSES_STDIN:
			asfd->parse_readbuf_specific=parse_readbuf_ncurses;
			break;
#endif
		default:
			logp("%s: unknown asfd stream type in %s: %d\n",
				desc, __func__, asfd->streamtype);
			return -1;
	}

	if(!(asfd->rbuf=iobuf_alloc())
	  || asfd_alloc_buf(&asfd->readbuf)
	  || asfd_alloc_buf(&asfd->writebuf)
	  || !(asfd->desc=strdup_w(desc, __func__)))
		return -1;
	return 0;
}

struct asfd *asfd_alloc(void)
{
	struct asfd *asfd;
	if(!(asfd=(struct asfd *)calloc_w(1, sizeof(struct asfd), __func__)))
		return NULL;
	asfd->init=asfd_init;
	return asfd;
}

void asfd_close(struct asfd *asfd)
{
	if(!asfd) return;
	if(asfd->ssl && asfd->fd>=0)
	{
		int r;
		set_blocking(asfd->fd);
		// I do not think this SSL_shutdown stuff works right.
		// Ignore it for now.
#ifndef HAVE_WIN32
		signal(SIGPIPE, SIG_IGN);
#endif
		if(!(r=SSL_shutdown(asfd->ssl)))
		{
			shutdown(asfd->fd, 1);
			r=SSL_shutdown(asfd->ssl);
		}
	}
	if(asfd->ssl)
	{
		SSL_free(asfd->ssl);
		asfd->ssl=NULL;
	}
	close_fd(&asfd->fd);
}

void asfd_free(struct asfd **asfd)
{
	if(!asfd || !*asfd) return;
	asfd_close(*asfd);
	iobuf_free(&((*asfd)->rbuf));
	free_w(&((*asfd)->readbuf));
	free_w(&((*asfd)->writebuf));
	free_w(&((*asfd)->desc));
	// FIX THIS: free incoming?
	blist_free(&((*asfd)->blist));
	free_v((void **)asfd);
}

struct asfd *setup_asfd(struct async *as, const char *desc, int *fd, SSL *ssl,
	enum asfd_streamtype asfd_streamtype, enum asfd_fdtype fdtype,
	pid_t pid, struct conf *conf)
{
	struct asfd *asfd=NULL;
	if(!fd || *fd<0)
	{
		logp("Given invalid descriptor in %s\n", __func__);
		goto error;
	}
	set_non_blocking(*fd);
	if(!(asfd=asfd_alloc())
	  || asfd->init(asfd, desc, as, *fd, ssl, asfd_streamtype, conf))
		goto error;
	asfd->fdtype=fdtype;
	asfd->pid=pid;
	*fd=-1;
	as->asfd_add(as, asfd);
	return asfd;
error:
	asfd_free(&asfd);
	return NULL;
}
