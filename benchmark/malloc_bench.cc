#include <stdlib.h>
#include <stdio.h>

int main(void)
{
	long long i = 1LL<<(28-4);
	size_t sz = 32;
	printf("i = %lld\n", i);
	for (;i>0;i--) {
		void *p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		p = malloc(sz);
		if (!p) {
			abort();
		}
		free(p);
		sz = ((sz | reinterpret_cast<size_t>(p)) & 511) + 16;
	}
	return 0;
}
