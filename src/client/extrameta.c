#include "include.h"

int has_extrameta(const char *path, char cmd, struct conf *conf)
{
#if defined(WIN32_VSS)
	return 1;
#endif
#if defined(HAVE_LINUX_OS) || \
    defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)

	// FIX THIS: extra meta not supported in burp2.
	if(conf->protocol==PROTO_BURP2) return 0;

#ifdef HAVE_ACL
	if(has_acl(path, cmd)) return 1;
#endif
#endif
#if defined(HAVE_LINUX_OS) || \
    defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)
#ifdef HAVE_XATTR
	if(has_xattr(path, cmd)) return 1;
#endif
#endif
        return 0;
}

int get_extrameta(struct asfd *asfd,
	BFILE *bfd,
	struct sbuf *sb,
	char **extrameta,
	size_t *elen,
	struct conf *conf)
{
#if defined (WIN32_VSS)
	if(get_vss(bfd, sb, extrameta, elen, conf)) return -1;
#endif
	// Important to do xattr directly after acl, because xattr is excluding
	// some entries if acls were set.
#if defined(HAVE_LINUX_OS) || \
    defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)
#ifdef HAVE_ACL
	if(get_acl(asfd, sb, extrameta, elen, conf)) return -1;
#endif
#endif
#if defined(HAVE_LINUX_OS) || \
    defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)
#ifdef HAVE_XATTR
	if(get_xattr(asfd, sb, extrameta, elen, conf)) return -1;
#endif
#endif
        return 0;
}

int set_extrameta(struct asfd *asfd,
	BFILE *bfd,
	const char *path,
	struct sbuf *sb,
	const char *extrameta,
	size_t metalen,
	struct conf *conf)
{
	size_t l=0;
	char cmdtmp='\0';
	unsigned int s=0;
	const char *metadata=NULL;
	int errors=0;

	metadata=extrameta;
	l=metalen;
	while(l>0)
	{
		char *m=NULL;
		if((sscanf(metadata, "%c%08X", &cmdtmp, &s))!=2)
		{
			logw(asfd, conf,
				"sscanf of metadata failed for %s: %s\n",
				path, metadata);
			return -1;
		}
		metadata+=9;
		l-=9;
		if(!(m=(char *)malloc_w(s+1, __func__)))
			return -1;
		memcpy(m, metadata, s);
		m[s]='\0';

		metadata+=s;
		l-=s;

		switch(cmdtmp)
		{
#if defined(HAVE_WIN32)
			case META_VSS:
				if(set_vss(bfd, m, s, conf)) errors++;
				break;
#endif
#if defined(HAVE_LINUX_OS) || \
    defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)
#ifdef HAVE_ACL
			case META_ACCESS_ACL:
				if(set_acl(asfd, path, sb, m, s, cmdtmp, conf))
					errors++;
				break;
			case META_DEFAULT_ACL:
				if(set_acl(asfd, path, sb, m, s, cmdtmp, conf))
					errors++;
				break;
#endif
#endif
#if defined(HAVE_LINUX_OS)
#ifdef HAVE_XATTR
			case META_XATTR:
				if(set_xattr(asfd,
					path, sb, m, s, cmdtmp, conf))
						errors++;
				break;
#endif
#endif
#if defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_OPENBSD_OS) || \
    defined(HAVE_NETBSD_OS)
#ifdef HAVE_XATTR
			case META_XATTR_BSD:
				if(set_xattr(asfd,
					path, statp, m, s, cmdtmp, conf))
						errors++;
				break;
#endif
#endif
			default:
				logp("unknown metadata: %c\n", cmdtmp);
				logw(asfd, conf,
					"unknown metadata: %c\n", cmdtmp);
				errors++;
				break;
				
		}
		free(m);
	}

	return errors;
}
