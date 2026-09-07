#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <alpm.h>
#include <alpm_list.h>
#include <stdio.h>
static double f_;
static double us(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart/f_;}
int main(void){
	LARGE_INTEGER f;QueryPerformanceFrequency(&f);f_=f.QuadPart/1e6;
	alpm_errno_t e=0;alpm_handle_t*h=alpm_initialize("/","/var/lib/pacman/",&e);
	alpm_db_t*db=alpm_get_localdb(h);
	alpm_list_t*c=alpm_db_get_pkgcache(db);
	size_t n=alpm_list_count(c);
	printf("  %zu packages\n\n", n);
	char rest[24];snprintf(rest,sizeof(rest),"rest (x%zu)",n?n-1:0);
	printf("  %-26s %10s %10s\n","field","first col",rest);
	#define COL(label, expr) do{                                    \
		double t0=us(); alpm_list_t*i=c; (void)(expr); double t1=us(); \
		for(i=i->next;i;i=i->next) (void)(expr);                   \
		double t2=us();                                            \
		printf("  %-26s %8.2f ms %8.3f ms\n",label,(t1-t0)/1000.0,(t2-t1)/1000.0); \
	}while(0)
	COL("alpm_pkg_get_name",    alpm_pkg_get_name((alpm_pkg_t*)i->data));
	COL("alpm_pkg_get_version", alpm_pkg_get_version((alpm_pkg_t*)i->data));
	COL("alpm_pkg_get_desc",    alpm_pkg_get_desc((alpm_pkg_t*)i->data));
	COL("alpm_pkg_get_url",     alpm_pkg_get_url((alpm_pkg_t*)i->data));
	COL("alpm_pkg_get_isize",   alpm_pkg_get_isize((alpm_pkg_t*)i->data));
	COL("alpm_pkg_get_builddate",alpm_pkg_get_builddate((alpm_pkg_t*)i->data));
	alpm_release(h);
	return 0;
}
