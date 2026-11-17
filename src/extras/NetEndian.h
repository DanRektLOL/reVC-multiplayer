#pragma once
#include <stdint.h>

#if defined(_XBOX) || defined(__PPC__) || defined(__ppc__) || defined(_M_PPC)
#define COOP_BIGENDIAN 1
#else
#define COOP_BIGENDIAN 0
#endif

inline void
CoopSwap(void *data, const char *layout, int times = 1)
{
#if COOP_BIGENDIAN
	unsigned char *p = (unsigned char *)data;
	for(int t = 0; t < times; t++) {
		for(const char *l = layout; *l;) {
			if(*l == ' ') {
				l++;
				continue;
			}
			int n = 0;
			while(*l >= '0' && *l <= '9') n = n * 10 + (*l++ - '0');
			if(n == 0) n = 1;
			char c = *l++;
			int sz = c == 'h' ? 2 : c == 'w' ? 4 : 1;
			for(int i = 0; i < n; i++, p += sz) {
				if(sz == 2) {
					unsigned char a = p[0];
					p[0] = p[1];
					p[1] = a;
				} else if(sz == 4) {
					unsigned char a = p[0], b = p[1];
					p[0] = p[3];
					p[1] = p[2];
					p[2] = b;
					p[3] = a;
				}
			}
		}
	}
#else
	(void)data;
	(void)layout;
	(void)times;
#endif
}